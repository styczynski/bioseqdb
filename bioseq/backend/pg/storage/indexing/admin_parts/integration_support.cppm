/**
 * @file integration_support.cppm
 * @brief PostgreSQL index administration fixtures and integrity audits.
 */
module;
#include <libassert/assert.hpp>
#include <bioseq/pg_compat.h>
#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/heapam.h"
#include "commands/tablespace.h"
#include "executor/tuptable.h"
#include "utils/inval.h"
#include "utils/snapmgr.h"
#include "utils/memutils.h"
extern "C" {
#include "storage/buffile.h"
}
#include <algorithm>
#include <array>
#include <concepts>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <expected>
#include <exception>
#include <limits>
#include <map>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>
export module bioseq.postgres.storage.indexing.admin:integration_support;
import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.postgres.support;
import bioseq.postgres.sql_boundary;
import bioseq.postgres.configuration.aligner;
import bioseq.postgres.configuration.core;
import bioseq.postgres.storage.manager;                     // seq_caches, sequence_locator, is_locator_stub, decode_locator_from_stub
import bioseq.storage.layout;
import bioseq.storage.size;
import bioseq.storage.lazy_sequence;                // make_resident_lazy_sequence
import bioseq.storage.scanner;             // sequence access requirements
import bioseq.postgres.storage.scanner;          // with_postgres_lazy_sequence
import bioseq.postgres.types.compact_sequence;   // bioseq::pg_types::seq, dispatch_instance
import bioseq.sequences;                   // compact_sequence_range
import bioseq.common.seqan_adapter;                      // seqan3::char_is_valid_for
import bioseq.indexing.type;
import bioseq.indexing.storage;
import bioseq.indexing.traits;
import bioseq.indexing.registry;
import bioseq.indexing.minimizer_hashmap;
import bioseq.indexing.hash_major_minimizer_index;
import bioseq.indexing.minimizer_sequence;
import bioseq.pipeline.runtime_contract;   // minimizer_algorithm
import bioseq.sequences.kmers;          // minimizer/index APIs
import bioseq.sequences.kmers.highway_minimizer_contract;
import bioseq.highway.dispatch_contract;
import bioseq.indexing.symbol_counts;
import bioseq.postgres.storage.indexing.chunked_storage;
import bioseq.postgres.storage.indexing.residency;
#ifdef BIOSEQ_TEST_INDEXES
import bioseq.postgres.storage.indexing.sorted_runs;
#endif
import bioseq.parallelism.memory_admission;
import bioseq.parallelism.mode;
import bioseq.parallelism.pool;
import bioseq.postgres.scheduler.supervisor;
import bioseq.postgres.scheduler.supervisor.vulkan;
import bioseq.postgres.table.io;
import bioseq.postgres.storage.indexing.handles;
import bioseq.postgres.column.table_access;
import bioseq.postgres.storage.metadata;                     // column_table_row_count
static_assert(__cplusplus >= 202302L, "C++23-or-later");
import :policy_catalog;
import :hash_major;
import :grouped_soa;
import :rebuild;

export namespace bioseq::storage::indexing {

/// result_type of the test seam that instantiates the production coordinator
/// against a real PostgreSQL-backed lazy sequence. The head and manifest are
/// independently reopened after publication; the route receipt is the
/// producer-originated execution result returned by the coordinator itself.
struct ready_minimizer_generation_coordinator_fixture_result {
    std::string table_name;
    std::string selected_backend_name;
    std::int16_t column_attno = 0;
    std::uint64_t sequence_rowno = 0;
    sequence_locator locator{};
    std::uint8_t alphabet_code = 0;
    std::uint8_t storage_unit_byte_width = 0;
    std::optional<ready_minimizer_generation_publication> publication;
    minimizer_generation_head head{};
    minimizer_generation_manifest manifest{};
    bool head_manifest_verified = false;

    /// Return the sealed publication retained by a completed fixture.
    [[nodiscard]] const ready_minimizer_generation_publication&
    sealed_publication() const noexcept {
        DEBUG_ASSERT(publication.has_value(),
            "READY fixture publication must be sealed before return");
        return *publication;
    }
};

/// Parse one exact fixture backend name without fallback.
[[nodiscard]] inline bioseq::minimizers::minimizer_sketch_backend
ready_minimizer_fixture_backend_from_name(std::string_view name) {
    using backend_type =
        bioseq::minimizers::minimizer_sketch_backend;
    if (name == "scalar") return backend_type::scalar;
    if (name == "exact_simd") return backend_type::exact_simd;
    if (name == "bounded_nthash_simd") return backend_type::bounded_nthash_simd;
    if (name == "vulkan_segment") return backend_type::vulkan_segment;
    bioseq::postgres::support::invoke_pg_as_cpp([name] {
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg("READY minimizer fixture backend is invalid"),
             errdetail("backend=%.*s", static_cast<int>(name.size()),
                name.data()),
             errhint("Use scalar, exact_simd, bounded_nthash_simd, or vulkan_segment.")));
    });
    pg_unreachable();
}

/// Parsed fixture backend and coordinated failure-injection request.
struct ready_minimizer_fixture_backend_request {
    bioseq::minimizers::minimizer_sketch_backend backend =
        bioseq::minimizers::minimizer_sketch_backend::scalar;
    minimizer_generation_failure_injection failure = minimizer_generation_failure_injection::none;
#ifdef BIOSEQ_TEST_INDEXES
    bioseq::storage::scanner_leaf_failure scanner_failure =
        bioseq::storage::scanner_leaf_failure::none;
    sorted_run_failure_injection sorted_run_failure =
        sorted_run_failure_injection::none;
    std::uint64_t sorted_run_failure_operation = 1u;
    std::uint64_t sorted_run_merge_reader_budget_bytes = 0u;
#endif
};

/// Parse a fixture backend or one supported failure scenario.
[[nodiscard]] inline ready_minimizer_fixture_backend_request
parse_ready_minimizer_fixture_backend_request(std::string_view name) {
#ifdef BIOSEQ_TEST_INDEXES
    using namespace std::string_view_literals;
    using scanner_failure = bioseq::storage::scanner_leaf_failure;
    constexpr std::array k_scanner_failures{
        std::pair{"scalar_fail_scanner_interrupt_before"sv,
            scanner_failure::interrupt_before},
        std::pair{"scalar_fail_scanner_interrupt_after"sv,
            scanner_failure::interrupt_after},
        std::pair{"scalar_fail_scanner_row_read_before"sv,
            scanner_failure::row_read_before},
        std::pair{"scalar_fail_scanner_row_read_after"sv,
            scanner_failure::row_read_after},
        std::pair{"scalar_fail_scanner_palloc_before"sv,
            scanner_failure::palloc_before},
        std::pair{"scalar_fail_scanner_palloc_after"sv,
            scanner_failure::palloc_after},
    };
    for (const auto& [backend_name, failure] : k_scanner_failures) {
        if (name == backend_name) {
            return {
                .backend = bioseq::minimizers::
                    minimizer_sketch_backend::scalar,
                .scanner_failure = failure,
            };
        }
    }
    if (name == "scalar_fail_sorted_compact_create_before_nth2") {
        return {
            .backend = bioseq::minimizers::
                minimizer_sketch_backend::scalar,
            .sorted_run_failure =
                sorted_run_failure_injection::compact_create_before,
            .sorted_run_failure_operation = 2u,
            // Three 32-Kbp source cores produce three input runs. A two-reader
            // budget forces two compaction outputs, so operation 2 occurs only
            // after output 1 completed and its two input handles were closed.
            .sorted_run_merge_reader_budget_bytes =
                std::uint64_t{64} << 10,
        };
    }
    constexpr std::array k_sorted_run_failures{
        std::pair{"scalar_fail_sorted_create_before"sv,
            sorted_run_failure_injection::create_before},
        std::pair{"scalar_fail_sorted_create_after"sv,
            sorted_run_failure_injection::create_after},
        std::pair{"scalar_fail_sorted_write_before"sv,
            sorted_run_failure_injection::write_before},
        std::pair{"scalar_fail_sorted_write_after"sv,
            sorted_run_failure_injection::write_after},
        std::pair{"scalar_fail_sorted_read_before"sv,
            sorted_run_failure_injection::read_before},
        std::pair{"scalar_fail_sorted_read_after"sv,
            sorted_run_failure_injection::read_after},
        std::pair{"scalar_fail_sorted_seek_before"sv,
            sorted_run_failure_injection::seek_before},
        std::pair{"scalar_fail_sorted_seek_after"sv,
            sorted_run_failure_injection::seek_after},
        std::pair{"scalar_fail_sorted_size_before"sv,
            sorted_run_failure_injection::size_before},
        std::pair{"scalar_fail_sorted_size_after"sv,
            sorted_run_failure_injection::size_after},
        std::pair{"scalar_fail_sorted_close_before"sv,
            sorted_run_failure_injection::close_before},
        std::pair{"scalar_fail_sorted_close_after"sv,
            sorted_run_failure_injection::close_after},
        std::pair{"scalar_fail_sorted_interrupt_before"sv,
            sorted_run_failure_injection::interrupt_before},
        std::pair{"scalar_fail_sorted_interrupt_after"sv,
            sorted_run_failure_injection::interrupt_after},
    };
    for (const auto& [backend_name, failure] : k_sorted_run_failures) {
        if (name == backend_name) {
            return {
                .backend = bioseq::minimizers::
                    minimizer_sketch_backend::scalar,
                .sorted_run_failure = failure,
            };
        }
    }
    if (name == "scalar_fail_pg_spill") {
        return {
            .backend = bioseq::minimizers::
                minimizer_sketch_backend::scalar,
            .failure = minimizer_generation_failure_injection::
                minimizer_sequence_spill,
        };
    }
    if (name == "scalar_fail_pg_merge") {
        return {
            .backend = bioseq::minimizers::
                minimizer_sketch_backend::scalar,
            .failure = minimizer_generation_failure_injection::
                minimizer_sequence_merge,
        };
    }
    if (name == "scalar_fail_pg_tts_create") {
        return {
            .backend = bioseq::minimizers::
                minimizer_sketch_backend::scalar,
            .failure = minimizer_generation_failure_injection::generation_tts_create,
        };
    }
    if (name == "scalar_fail_pg_tuple_form") {
        return {
            .backend = bioseq::minimizers::
                minimizer_sketch_backend::scalar,
            .failure = minimizer_generation_failure_injection::generation_tuple_form,
        };
    }
    if (name == "scalar_fail_pg_tts_store") {
        return {
            .backend = bioseq::minimizers::
                minimizer_sketch_backend::scalar,
            .failure = minimizer_generation_failure_injection::generation_tts_store,
        };
    }
    if (name == "scalar_fail_pg_tts_drop") {
        return {
            .backend = bioseq::minimizers::
                minimizer_sketch_backend::scalar,
            .failure = minimizer_generation_failure_injection::generation_tts_drop,
        };
    }
    if (name == "scalar_fail_pg_multi_insert") {
        return {
            .backend = bioseq::minimizers::
                minimizer_sketch_backend::scalar,
            .failure = minimizer_generation_failure_injection::generation_multi_insert,
        };
    }
    if (name == "scalar_fail_pg_reread") {
        return {
            .backend = bioseq::minimizers::
                minimizer_sketch_backend::scalar,
            .failure = minimizer_generation_failure_injection::generation_reread,
        };
    }
    if (name == "scalar_fail_pg_spi_publish") {
        return {
            .backend = bioseq::minimizers::
                minimizer_sketch_backend::scalar,
            .failure = minimizer_generation_failure_injection::generation_spi_publish,
        };
    }
    if (name == "scalar_fail_pg_writer_close") {
        return {
            .backend = bioseq::minimizers::
                minimizer_sketch_backend::scalar,
            .failure = minimizer_generation_failure_injection::generation_writer_close,
        };
    }
    if (name == "scalar_fail_pg_storage_close") {
        return {
            .backend = bioseq::minimizers::
                minimizer_sketch_backend::scalar,
            .failure = minimizer_generation_failure_injection::generation_storage_close,
        };
    }
    if (name == "scalar_fail_pg_post_publish_reread") {
        return {
            .backend = bioseq::minimizers::
                minimizer_sketch_backend::scalar,
            .failure = minimizer_generation_failure_injection::
                generation_post_publish_reread,
        };
    }
    if (name == "scalar_fail_pg_reader_close") {
        return {
            .backend = bioseq::minimizers::
                minimizer_sketch_backend::scalar,
            .failure = minimizer_generation_failure_injection::generation_reader_close,
        };
    }
#endif
    return {
        .backend = ready_minimizer_fixture_backend_from_name(name),
        .failure = minimizer_generation_failure_injection::none,
    };
}

/// Arm every failure surface selected by one fixture request.
inline void arm_ready_minimizer_fixture_failures(
        const ready_minimizer_fixture_backend_request& request) noexcept {
    arm_minimizer_generation_failure_injection(request.failure);
#ifdef BIOSEQ_TEST_INDEXES
    bioseq::storage::arm_scanner_pg_leaf_failure(
        request.scanner_failure);
    arm_sorted_run_pg_leaf_failure_on_nth(
        request.sorted_run_failure,
        request.sorted_run_failure_operation);
    set_sorted_run_merge_reader_budget_for_test(
        request.sorted_run_merge_reader_budget_bytes);
#endif
}

/// Clear fixture failure injection and merge-budget overrides.
inline void disarm_ready_minimizer_fixture_failures() noexcept {
    disarm_minimizer_generation_failure_injection();
#ifdef BIOSEQ_TEST_INDEXES
    bioseq::storage::disarm_scanner_pg_leaf_failure();
    disarm_sorted_run_pg_leaf_failure();
#endif
}

/// Integration-only direct execution of the production grouped-SoA coordinator.
/// This deliberately bypasses the ordinary update route and does not touch
/// metadata_age_log. The caller selects the biological backend
/// explicitly; an unavailable SIMD/GPU route fails instead of falling back.
[[nodiscard]] inline ready_minimizer_generation_coordinator_fixture_result
build_ready_minimizer_generation_coordinator_fixture_impl(
        Oid rel_oid,
        std::string_view index_name,
        std::uint64_t sequence_rowno,
        std::string_view backend_name) {
    using backend_type =
        bioseq::minimizers::minimizer_sketch_backend;
    constexpr std::uint16_t k_kmer_size = 15u;
    constexpr std::uint16_t k_window_size = 10u;

    const bool is_bioseq_table = OidIsValid(rel_oid)
        && bioseq::postgres::support::invoke_pg_as_cpp(
            [rel_oid] { return relation_oid_uses_column_table_access_method(rel_oid); });
    if (!is_bioseq_table) {
        ready_minimizer_error_as_cpp(
            ERRCODE_WRONG_OBJECT_TYPE,
            "READY minimizer fixture requires a USING bioseq table");
    }
    if (!superuser()) {
        ready_minimizer_error_as_cpp(
            ERRCODE_INSUFFICIENT_PRIVILEGE,
            "READY minimizer fixture requires superuser");
    }
    if (index_name != k_minimizers_per_fragment_index_name
        && index_name != k_minimizers_per_sequence_index_name) {
        ready_minimizer_error_as_cpp(
            ERRCODE_INVALID_PARAMETER_VALUE,
            "READY minimizer fixture requires minimizers_per_fragment or minimizers_per_sequence");
    }
    if (!bioseq::indexing::sql_bigint_can_represent(sequence_rowno)) {
        ready_minimizer_error_as_cpp(
            ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE,
            "READY minimizer fixture row number exceeds BIGINT");
    }
    const ready_minimizer_fixture_backend_request backend_request =
        parse_ready_minimizer_fixture_backend_request(backend_name);
    const backend_type backend = backend_request.backend;
    const bioseq::minimizers::minimizer_kernels* const minimizer_kernels =
        selected_minimizer_kernels_for_operation(
            backend, "READY minimizer coordinator fixture");
    Snapshot active_snapshot = GetActiveSnapshot();
    if (active_snapshot == nullptr || !IsMVCCSnapshot(active_snapshot)) {
        ready_minimizer_error_as_cpp(
            ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
            "READY minimizer fixture requires an active MVCC snapshot");
    }

    const std::int16_t seq_attno = bioseq::postgres::support::invoke_pg_as_cpp(
        [rel_oid] { return single_seq_attno_for_inspection(rel_oid); });
    const relation_identity relation_identity =
        bioseq::postgres::support::invoke_pg_as_cpp(
            [rel_oid] { return index_relation_identity(rel_oid); });
    if (relation_identity.db_oid != MyDatabaseId
        || !OidIsValid(relation_identity.tablespace_oid)
        || !OidIsValid(relation_identity.relfilenode)) {
        ready_minimizer_error_as_cpp(
            ERRCODE_UNDEFINED_TABLE,
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
        std::optional<index_storage_fragment_generation_read_session>
            verification_session;
        std::optional<ready_minimizer_generation_coordinator_fixture_result>
            result;
        bool completed = false;
    };
    auto* state = new (std::nothrow) fixture_state{};
    if (state == nullptr) {
        ready_minimizer_error_as_cpp(
            ERRCODE_OUT_OF_MEMORY,
            "READY minimizer fixture could not allocate cleanup state");
    }
    const MemoryContext caller_context = CurrentMemoryContext;

    try {
        state->result.emplace(
            ready_minimizer_generation_coordinator_fixture_result{
                .selected_backend_name = bioseq::minimizers::
                    minimizer_sketch_backend_name(backend),
                .column_attno = seq_attno,
                .sequence_rowno = sequence_rowno,
            });
        ready_minimizer_generation_coordinator_fixture_result& result =
            *state->result;
        state->source_snapshot = bioseq::postgres::support::invoke_pg_as_cpp(
            [active_snapshot] { return RegisterSnapshot(active_snapshot); });
        if (state->source_snapshot == nullptr
            || state->source_snapshot->regd_count == 0) {
            ready_minimizer_error_as_cpp(
                ERRCODE_INTERNAL_ERROR,
                "READY minimizer fixture could not register its source snapshot");
        }
        state->work_context = bioseq::postgres::support::invoke_pg_as_cpp(
            [caller_context] {
                return AllocSetContextCreate(
                    caller_context,
                    "bioseq READY minimizer coordinator fixture",
                    ALLOCSET_DEFAULT_SIZES);
            });
        state->source_relation = bioseq::postgres::support::invoke_pg_as_cpp(
            [rel_oid] {
                return bioseq::postgres::support::try_open_postgres_relation(
                    rel_oid, AccessShareLock);
            });
        if (state->source_relation == nullptr
            || RelationGetRelid(state->source_relation) != rel_oid
            || state->source_relation->rd_locator.dbOid
                != relation_identity.db_oid
            || state->source_relation->rd_locator.spcOid
                != relation_identity.tablespace_oid
            || static_cast<Oid>(
                state->source_relation->rd_locator.relNumber)
                != relation_identity.relfilenode) {
            ready_minimizer_error_as_cpp(
                ERRCODE_UNDEFINED_TABLE,
                "READY minimizer fixture source relation identity changed");
        }

        TupleDesc source_desc = RelationGetDescr(state->source_relation);
        MemoryContext previous_context =
            bioseq::postgres::support::invoke_pg_as_cpp(
                [state] {
                    return MemoryContextSwitchTo(state->work_context);
                });
        Datum* values = bioseq::postgres::support::invoke_pg_as_cpp(
            [source_desc] {
                return static_cast<Datum*>(palloc0(
                    sizeof(Datum)
                    * static_cast<std::size_t>(source_desc->natts)));
            });
        bool* nulls = bioseq::postgres::support::invoke_pg_as_cpp(
            [source_desc] {
                return static_cast<bool*>(palloc0(
                    sizeof(bool)
                    * static_cast<std::size_t>(source_desc->natts)));
            });
        List* projected_columns = bioseq::postgres::support::invoke_pg_as_cpp(
            [seq_attno] { return list_make1_int(seq_attno); });
        state->source_read = bioseq::postgres::support::invoke_pg_as_cpp(
            [state, source_desc, projected_columns] {
                return begin_column_read(
                    state->source_relation, source_desc,
                    projected_columns, NIL, state->work_context,
                    state->source_snapshot, /*randomAccess=*/true, nullptr);
            });
        const bool row_visible = bioseq::postgres::support::invoke_pg_as_cpp(
            [state, sequence_rowno, values, nulls] {
                return read_column_row_by_number(
                    state->source_read, sequence_rowno, values, nulls);
            });
        if (!row_visible || nulls[seq_attno - 1]) {
            bioseq::postgres::support::invoke_pg_as_cpp(
                [previous_context] {
                    MemoryContextSwitchTo(previous_context);
                });
            ready_minimizer_error_as_cpp(
                ERRCODE_NO_DATA_FOUND,
                "READY minimizer fixture source row is not visible",
                fmt::format("sequence_rowno={}", sequence_rowno));
        }
        bioseq::pg_types::seq* source_header = bioseq::postgres::support::invoke_pg_as_cpp(
            [values, seq_attno] {
                return reinterpret_cast<bioseq::pg_types::seq*>(
                    PG_DETOAST_DATUM(values[seq_attno - 1]));
            });
        if (!is_locator_stub(source_header)) {
            bioseq::postgres::support::invoke_pg_as_cpp(
                [previous_context] {
                    MemoryContextSwitchTo(previous_context);
                });
            ready_minimizer_error_as_cpp(
                ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
                "READY minimizer fixture requires a fragmented PostgreSQL-backed source");
        }
        result.locator = decode_locator_from_stub(source_header);
        result.alphabet_code =
            static_cast<std::uint8_t>(source_header->alphabet_code);
        result.storage_unit_byte_width =
            logical_storage_unit_byte_width(source_header);
        if (result.locator.payload_version != sequence_locator_payload_version
            || result.locator.source_epoch == 0
            || result.locator.logical_length == 0
            || result.locator.fragment_count == 0) {
            bioseq::postgres::support::invoke_pg_as_cpp(
                [previous_context] {
                    MemoryContextSwitchTo(previous_context);
                });
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "READY minimizer fixture locator is invalid");
        }
        bioseq::postgres::support::invoke_pg_as_cpp(
            [state] { end_column_read(state->source_read); });
        state->source_read = nullptr;
        bioseq::postgres::support::invoke_pg_as_cpp(
            [previous_context] {
                MemoryContextSwitchTo(previous_context);
            });
        Relation finished_source = std::exchange(
            state->source_relation, nullptr);
        bioseq::postgres::support::invoke_pg_as_cpp(
            [finished_source] {
                relation_close(finished_source, AccessShareLock);
            });

        state->fragment_caches = bioseq::postgres::support::invoke_pg_as_cpp(
            [state] { return alloc_seq_caches(state->work_context); });
        state->fragment_caches->side = seq_caches::access_side::read;
        state->fragment_caches->lock_mode = AccessShareLock;
        state->fragment_caches->read_state_mctx = state->work_context;
        state->fragment_caches->main_database_oid =
            relation_identity.db_oid;
        state->fragment_caches->main_tablespace_oid =
            relation_identity.tablespace_oid;
        state->fragment_caches->main_relfilenode =
            relation_identity.relfilenode;

        bioseq::pg_types::seq::dispatch_instance(
                result.alphabet_code, result.storage_unit_byte_width,
                [state, rel_oid, relation_identity, seq_attno, index_name,
                 sequence_rowno, backend, backend_request,
                 minimizer_kernels, &result]<class p_sequence_config>() {
                    using source_binding =
                        bioseq::storage::postgres_lazy_sequence_binding<
                            p_sequence_config>;
                    const source_binding bound_source{
                        relation_identity.db_oid,
                        relation_identity.tablespace_oid,
                        relation_identity.relfilenode, seq_attno,
                        result.locator,
                        source_binding::expected_last_fragment_padding(
                            result.locator),
                        state->source_snapshot, state->fragment_caches,
                        state->work_context};
                    auto opened_storage =
                        bioseq::postgres::support::invoke_pg_as_cpp(
                            [state, rel_oid, relation_identity, seq_attno,
                             index_name, sequence_rowno, &result] {
                                return open_fragment_storage(
                                    rel_oid, relation_identity.db_oid,
                                    relation_identity.tablespace_oid,
                                    relation_identity.relfilenode, seq_attno,
                                    index_name, result.locator,
                                    sequence_rowno, state->fragment_caches,
                                    state->work_context);
                            });
                    state->storage.emplace(std::move(opened_storage));
                    result.table_name = state->storage->this_table_name();
                    bioseq::storage::with_postgres_lazy_sequence<p_sequence_config>(
                        bound_source,
                        [state, rel_oid, seq_attno, index_name,
                         sequence_rowno, backend, backend_request,
                         minimizer_kernels, &result,
                         bound_source](auto lazy_view) {
                            arm_ready_minimizer_fixture_failures(
                                backend_request);
                            result.publication.emplace(
                                build_and_publish_grouped_soa_minimizer_generation_pg(
                                    rel_oid, seq_attno, index_name,
                                    sequence_rowno, *state->storage,
                                    lazy_view, bound_source, backend,
                                    minimizer_kernels));
                            disarm_ready_minimizer_fixture_failures();
                        });
                });
        const bool execution_kind_matches = result.publication.has_value()
            && (index_name == k_minimizers_per_fragment_index_name
                ? std::holds_alternative<
                    bioseq::indexing::minimizer_fragment_generation_execution_receipt>(
                        result.publication->execution())
                : std::holds_alternative<
                    bioseq::indexing::minimizer_sequence_generation_execution_receipt>(
                        result.publication->execution()));
        if (!result.publication.has_value()
            || !result.publication->published()
            || !result.publication->execution_accepted()
            || !execution_kind_matches
            || result.publication->selected_backend() != backend) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "READY minimizer fixture publication validation was rejected");
        }

        state->storage->close_checked();
        state->storage.reset();
        bioseq::postgres::support::invoke_pg_as_cpp([state] {
            close_and_destroy_seq_caches(
                state->fragment_caches, /*commit=*/false);
        });
        state->fragment_caches = nullptr;
        bioseq::postgres::support::invoke_pg_as_cpp(
            [state] { UnregisterSnapshot(state->source_snapshot); });
        state->source_snapshot = nullptr;

        // Reopen the published head and immutable manifest independently.  A
        // fresh registered copy validates that the rows are visible
        // through PostgreSQL after the private READY token was consumed.
        bioseq::postgres::support::invoke_pg_as_cpp([] {
            CommandCounterIncrement();
        });
        bioseq::postgres::support::invoke_pg_as_cpp([] {
            UpdateActiveSnapshotCommandId();
        });
        Snapshot verification_source = GetActiveSnapshot();
        state->verification_snapshot =
            bioseq::postgres::support::invoke_pg_as_cpp(
                [verification_source] {
                    return RegisterSnapshot(verification_source);
                });
        const auto expected = bioseq::indexing::
            expected_grouped_soa_generation_identity_for_backend(
                bioseq::indexing::minimizer_generation_layout::
                    grouped_soa_segment_local,
                backend, result.alphabet_code,
                result.storage_unit_byte_width, k_kmer_size, k_window_size,
                bioseq::indexing::k_grouped_soa_default_max_occurrence_cap);
        if (!expected) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "READY minimizer fixture could not derive its read contract");
        }
        state->verification_session.emplace(
            result.table_name, sequence_rowno,
            state->verification_snapshot, *expected,
            relation_identity.tablespace_oid,
            minimizer_generation_snapshot_registration::borrow_registered);
        result.head = state->verification_session->read_active_head_once();
        result.manifest = state->verification_session
            ->read_manifest_directory_once(result.head).manifest;
        const std::uint64_t manifest_storage_contract = bioseq::indexing::
            compute_minimizer_generation_storage_contract_checksum(
                result.manifest);
        const auto scheme = bioseq::indexing::
            minimizer_scheme_identity_for_backend(backend);
        const auto producer_backend = bioseq::indexing::
            minimizer_producer_backend_identity_for_backend(backend);
        if (result.head.layout
                != bioseq::indexing::minimizer_generation_layout::
                    grouped_soa_segment_local
            || result.head.generation_id
                != result.publication->generation_id()
            || result.head.source_identity
                != result.publication->source_identity()
            || result.head.source_content_checksum
                != result.publication->source_content_checksum()
            || result.head.storage_contract_checksum
                != result.publication->storage_contract_checksum()
            || result.manifest.layout != result.head.layout
            || result.manifest.generation_id != result.head.generation_id
            || result.manifest.source_identity != result.head.source_identity
            || result.manifest.source_content_checksum
                != result.head.source_content_checksum
            || result.manifest.target_length
                != result.locator.logical_length
            || result.manifest.control_count
                != result.publication->control_count()
            || result.manifest.position_count
                != result.publication->position_count()
            || result.manifest.kmer_size != k_kmer_size
            || result.manifest.window_size != k_window_size
            || result.manifest.minimizer_scheme_id != scheme.id
            || result.manifest.minimizer_scheme_revision != scheme.revision
            || result.manifest.producer_backend_id != producer_backend.id
            || result.manifest.producer_identity_revision
                != producer_backend.revision
            || manifest_storage_contract
                != result.publication->storage_contract_checksum()) {
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

        MemoryContext finished_context = std::exchange(
            state->work_context, nullptr);
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
        ready_minimizer_error_as_cpp(
            ERRCODE_INTERNAL_ERROR,
            "READY minimizer fixture did not complete or report an error");
    }
    ready_minimizer_generation_coordinator_fixture_result result =
        std::move(*state->result);
    state->result.reset();
    delete state;
    state = nullptr;
    return result;
}

/// Live PostgreSQL fixture result for the grouped-SoA storage contract. This
/// is used only by the integration SQL seam below; production readers never
/// call it, and only the current grouped-SoA layout is accepted.
struct minimizer_generation_integration_fixture_result {
    std::string table_name;
    bioseq::indexing::minimizer_generation_layout layout =
        bioseq::indexing::minimizer_generation_layout::invalid;
    std::uint64_t generation_id = 0;
    std::uint64_t source_identity = 0;
    std::uint64_t source_content_checksum = 0;
    std::uint64_t storage_contract_checksum = 0;
    std::uint64_t source_num_fragments = 0;
    std::uint64_t source_total_bp = 0;
    std::uint64_t source_padding_letters = 0;
    std::uint64_t writer_rows = 0;
    std::uint64_t writer_batches = 0;
    std::uint64_t published_head_bytes = 0;
    std::uint64_t control_count = 0;
    std::uint64_t position_count = 0;
    std::uint64_t directory_entries = 0;
    std::uint64_t position_segment_count = 0;
    std::uint64_t control_encoded_bytes = 0;
    std::uint64_t position_encoded_bytes = 0;
    std::uint64_t control_chunks = 0;
    std::uint64_t position_chunks = 0;
    std::uint64_t validated_control_bytes = 0;
    std::uint64_t validated_position_bytes = 0;
    std::uint64_t validated_control_checksum = 0;
    std::uint64_t validated_position_checksum = 0;
    std::uint64_t lookup_count = 0;
    std::uint64_t lookup_bp = 0;
    bool lookup_reverse = false;
    bool positions_complete = false;
    minimizer_generation_read_stats read_stats{};
    minimizer_generation_read_stats selective_probe_stats{};
};

/// Integration-only validation result for the one-scan full-generation payload.
struct minimizer_generation_full_payload_fixture_result {
    std::string table_name;
    std::string selected_backend_name;
    std::uint64_t generation_id = 0;
    std::uint64_t control_bytes = 0;
    std::uint64_t position_bytes = 0;
    std::uint64_t control_checksum = 0;
    std::uint64_t position_checksum = 0;
    minimizer_generation_read_stats read_stats{};
};

/// Integration-only observations for the generation-storage lifecycle. Optional
/// fields are emitted as SQL NULL by the fmgr wrapper when an operation does not
/// exercise that part of the contract.
struct minimizer_generation_storage_state_fixture_result {
    Oid before_oid = InvalidOid;
    Oid operation_oid = InvalidOid;
    Oid after_oid = InvalidOid;
    std::optional<bool> same_handle_rebound;
    std::optional<bool> generation_schema_after;
    std::optional<bool> chunk_cache_hit;
    std::optional<bool> assembled_cache_hit;
};

/// Deterministic replayable producer used by the live PG fixture. Its declared
/// scheme is the caller-requested backend identity. The Vulkan-labelled
/// storage fixture deliberately emits up to 32768 events with small valid
/// cores to exercise multi-entry/multi-segment scanning; other fixtures use a
/// 4096-event bound. No path retains biological source bytes.
struct minimizer_generation_fixture_producer {
    std::uint64_t target_length = 0;
    bioseq::minimizers::minimizer_sketch_backend backend =
        bioseq::minimizers::minimizer_sketch_backend::scalar;

    /// Return the persisted minimizer scheme selected by the fixture backend.
    [[nodiscard]] bioseq::indexing::minimizer_scheme_identity
    scheme_identity() const noexcept {
        return bioseq::indexing::minimizer_scheme_identity_for_backend(
            backend);
    }

    /// Return the producer identity selected by the fixture backend.
    [[nodiscard]] bioseq::indexing::minimizer_producer_identity
    producer_backend_identity() const noexcept {
        return bioseq::indexing::
            minimizer_producer_backend_identity_for_backend(backend);
    }

    /// Replay deterministic globally sorted events into one receiver.
    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    scan_sorted_events(
            std::uint64_t,
            bioseq::indexing::canonical_minimizer_event_receiver receiver) {
        const std::uint64_t fixture_event_limit =
            backend == bioseq::minimizers::
                minimizer_sketch_backend::vulkan_segment
            ? 32768u : 4096u;
        const std::uint64_t event_count = std::min<std::uint64_t>(
            target_length, fixture_event_limit);
        const bool spread_across_target = event_count > 1
            && backend == bioseq::minimizers::
                minimizer_sketch_backend::vulkan_segment;
        const std::uint64_t position_divisor = spread_across_target
            ? event_count - 1u : 1u;
        const std::uint64_t position_quotient = spread_across_target
            ? (target_length - 1u) / position_divisor : 0u;
        const std::uint64_t position_remainder = spread_across_target
            ? (target_length - 1u) % position_divisor : 0u;
        for (std::uint64_t event = 0; event < event_count; ++event) {
            // Exact endpoint-preserving interpolation without multiplying two
            // source-sized u64 values. The final event is target_length-1, so
            // any canonical grouped-SoA segment boundary is genuinely crossed.
            const std::uint64_t position = spread_across_target
                ? event * position_quotient
                    + (event * position_remainder) / position_divisor
                : event;
            auto emitted = receiver(
                bioseq::indexing::canonical_minimizer_event{
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

static_assert(bioseq::indexing::grouped_soa_event_producer<
    minimizer_generation_fixture_producer>);

/// Build and publish one exact live grouped-SoA generation. Every write
/// goes through the production row sink; this half deliberately opens no read
/// session so a later SQL statement/backend can validate an independently
/// reopened reader without inheriting session-owned state.
[[nodiscard]] inline minimizer_generation_integration_fixture_result
build_minimizer_generation_integration_fixture_impl(
        Oid rel_oid,
        std::string_view index_name,
        std::uint64_t sequence_rowno,
        std::uint16_t kmer_size,
        std::uint16_t window_size,
        bioseq::indexing::minimizer_generation_layout layout,
        bioseq::minimizers::minimizer_sketch_backend backend,
        std::uint64_t position_segment_span_bp) {
    if (!OidIsValid(rel_oid)
        || !bioseq::postgres::support::invoke_pg_as_cpp(
            [rel_oid] { return relation_oid_uses_column_table_access_method(rel_oid); })) {
        ready_minimizer_error_as_cpp(
            ERRCODE_WRONG_OBJECT_TYPE,
            "bioseq minimizer generation fixture requires a USING bioseq table");
    }
    if (!bioseq::postgres::support::invoke_pg_as_cpp([] { return superuser(); })) {
        ready_minimizer_error_as_cpp(
            ERRCODE_INSUFFICIENT_PRIVILEGE,
            "bioseq minimizer generation fixture requires superuser");
    }
    if (index_name != k_minimizers_per_fragment_index_name
        && index_name != k_minimizers_per_sequence_index_name) {
        ready_minimizer_error_as_cpp(
            ERRCODE_INVALID_PARAMETER_VALUE,
            "bioseq minimizer generation fixture requires a minimizer index");
    }
    if (kmer_size == 0 || window_size == 0
        || !bioseq::indexing::sql_bigint_can_represent(sequence_rowno)) {
        ready_minimizer_error_as_cpp(
            ERRCODE_INVALID_PARAMETER_VALUE,
            "bioseq minimizer generation fixture parameters are invalid");
    }
    if (layout
            != bioseq::indexing::minimizer_generation_layout::
                grouped_soa_segment_local) {
        ready_minimizer_error_as_cpp(
            ERRCODE_INVALID_PARAMETER_VALUE,
            "bioseq generation fixture requires the grouped-SoA layout");
    }
    const auto requested_scheme =
        bioseq::indexing::minimizer_scheme_identity_for_backend(backend);

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
        Snapshot active_snapshot = bioseq::postgres::support::invoke_pg_as_cpp(
            [] { return GetActiveSnapshot(); });
        if (active_snapshot == nullptr || !IsMVCCSnapshot(active_snapshot)) {
            ready_minimizer_error_as_cpp(
                ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
                "bioseq minimizer generation fixture requires an active MVCC snapshot");
        }
        state->registered_snapshot = bioseq::postgres::support::invoke_pg_as_cpp(
            [active_snapshot] { return RegisterSnapshot(active_snapshot); });
        state->setup_authorization.emplace(
            authorize_minimizer_generation_source_pg(
                identity, sequence_rowno, state->registered_snapshot));
        minimizer_generation_source_locator source =
            state->setup_authorization->source_for_diagnostics();
        const std::uint64_t source_content_checksum =
            state->setup_authorization->source_content_checksum_for_diagnostics();
        result.table_name = format_seqidx_name(
            index_name, rid.db_oid, rid.tablespace_oid, rid.relfilenode,
            seq_attno, source.locator.bucket_index, source.locator.shard_index);
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
            [&tablespace_name] {
                return quote_identifier(tablespace_name.c_str());
            });
        const std::string drop_ddl = fmt::format(
            "DROP TABLE IF EXISTS bioseq_internal.{}",
            quoted_table);
        bioseq::postgres::support::invoke_pg_as_cpp(
            [&drop_ddl] { run_internal_ddl(drop_ddl.c_str()); });
        const std::string create_ddl = fmt::format(
            "CREATE UNLOGGED TABLE bioseq_internal.{} ("
            "query_key BIGINT NOT NULL, generation_id BIGINT NOT NULL, "
            "position_segment INTEGER NOT NULL, sub_key BIGINT NOT NULL, "
            "chunk_no INTEGER NOT NULL, chunk_bytes INTEGER NOT NULL, "
            "checksum BIGINT NOT NULL, blob BYTEA NOT NULL, "
            "PRIMARY KEY (generation_id,query_key,sub_key,position_segment,chunk_no) "
            "WITH (fillfactor=90) USING INDEX TABLESPACE {}) "
            "USING heap WITH (fillfactor=100) TABLESPACE {}",
            quoted_table, quoted_tablespace, quoted_tablespace);
        bioseq::postgres::support::invoke_pg_as_cpp(
            [&create_ddl] { run_internal_ddl(create_ddl.c_str()); });
        const std::string alter_ddl = fmt::format(
            "ALTER TABLE bioseq_internal.{} ALTER COLUMN blob SET STORAGE PLAIN",
            quoted_table);
        bioseq::postgres::support::invoke_pg_as_cpp(
            [&alter_ddl] { run_internal_ddl(alter_ddl.c_str()); });
        bioseq::postgres::support::invoke_pg_as_cpp(
            [] { CommandCounterIncrement(); });

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

        state->writer.emplace(
            identity, sequence_rowno, result.generation_id,
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
        };
        std::optional<minimizer_generation_encoding_error> encode_error;
        {
            auto encoded = bioseq::indexing::encode_minimizer_generation(
                producer, *state->writer, options);
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
                fmt::format("wire_error={}",
                    static_cast<unsigned>(*encode_error)));
        }
        result.storage_contract_checksum =
            bioseq::indexing::compute_minimizer_generation_storage_contract_checksum(
                state->generation->manifest);
        state->ready_token.emplace(
            state->writer->validate_ready_generation(*state->generation));
        result.writer_rows = state->writer->row_count();
        result.writer_batches =
            state->writer->catalog_multi_insert_batches();
        [[maybe_unused]] ready_minimizer_generation_published_head_receipt receipt =
            publish_ready_minimizer_generation_head_pg(
            std::move(*state->ready_token));
        state->ready_token.reset();
        result.published_head_bytes = minimizer_generation_head::encoded_bytes;

        result.control_count = state->generation->manifest.control_count;
        result.position_count = state->generation->manifest.position_count;
        result.directory_entries =
            state->generation->directory.entries().size();
        result.position_segment_count =
            state->generation->manifest.position_segment_count;
        const std::uint64_t expected_event_count =
            std::min<std::uint64_t>(target_length,
                backend == bioseq::minimizers::
                    minimizer_sketch_backend::vulkan_segment
                ? 32768u : 4096u);
        if (result.control_count != expected_event_count
            || result.position_count != expected_event_count) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "bioseq minimizer generation fixture event totals disagree");
        }
        for (const grouped_soa_bucket_directory_entry& entry
             : state->generation->directory.entries()) {
            result.control_encoded_bytes =
                bioseq::common::saturating_add(
                    result.control_encoded_bytes,
                    entry.control_encoded_bytes);
            result.position_encoded_bytes =
                bioseq::common::saturating_add(
                    result.position_encoded_bytes,
                    entry.position_encoded_bytes);
            result.control_chunks =
                bioseq::common::saturating_add(
                    result.control_chunks,
                    entry.control_chunk_count);
            result.position_chunks =
                bioseq::common::saturating_add(
                    result.position_chunks,
                    entry.position_chunk_count);
        }
        if (result.control_encoded_bytes
                == (std::numeric_limits<std::uint64_t>::max)()
            || result.position_encoded_bytes
                == (std::numeric_limits<std::uint64_t>::max)()
            || result.control_chunks
                == (std::numeric_limits<std::uint64_t>::max)()
            || result.position_chunks
                == (std::numeric_limits<std::uint64_t>::max)()) {
            ready_minimizer_error_as_cpp(
                ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE,
                "bioseq minimizer generation fixture payload totals overflow");
        }
        state->generation.reset();
        state->writer.reset();
        bioseq::postgres::support::invoke_pg_as_cpp(
            [] { CommandCounterIncrement(); });
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
            Snapshot cleanup = std::exchange(
                state->registered_snapshot, nullptr);
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

/// Reopen and consume the active fixture generation under one independently
/// registered MVCC snapshot. The selected wire format chooses the qualified
/// host reader only; this integration seam does not participate in mmsearch.
[[nodiscard]] inline minimizer_generation_integration_fixture_result
read_minimizer_generation_integration_fixture_impl(
		Oid rel_oid,
		std::string_view index_name,
		std::uint64_t sequence_rowno,
		std::uint16_t kmer_size,
		std::uint16_t window_size,
		bioseq::indexing::minimizer_generation_layout layout,
		bioseq::minimizers::minimizer_sketch_backend backend) {
    if (!OidIsValid(rel_oid)
        || !bioseq::postgres::support::invoke_pg_as_cpp(
            [rel_oid] { return relation_oid_uses_column_table_access_method(rel_oid); })) {
        ready_minimizer_error_as_cpp(
            ERRCODE_WRONG_OBJECT_TYPE,
            "bioseq minimizer generation fixture reader requires a USING bioseq table");
    }
    if (!bioseq::postgres::support::invoke_pg_as_cpp([] { return superuser(); })) {
        ready_minimizer_error_as_cpp(
            ERRCODE_INSUFFICIENT_PRIVILEGE,
            "bioseq minimizer generation fixture reader requires superuser");
    }
    if (index_name != k_minimizers_per_fragment_index_name
        && index_name != k_minimizers_per_sequence_index_name) {
        ready_minimizer_error_as_cpp(
            ERRCODE_INVALID_PARAMETER_VALUE,
            "bioseq minimizer generation fixture reader requires a minimizer index");
    }
    if (kmer_size == 0 || window_size == 0
        || !bioseq::indexing::sql_bigint_can_represent(sequence_rowno)
        || layout
            != bioseq::indexing::minimizer_generation_layout::
                grouped_soa_segment_local) {
        ready_minimizer_error_as_cpp(
            ERRCODE_INVALID_PARAMETER_VALUE,
            "bioseq minimizer generation fixture reader parameters are invalid");
    }

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

    struct fixture_read_state {
        Snapshot registered_snapshot = nullptr;
        std::optional<minimizer_generation_source_authorization> source_authorization;
        std::optional<minimizer_generation_catalog_source_authorization>
            catalog_source_authorization;
        std::optional<index_storage_fragment_generation_read_session> session;
        std::optional<bioseq::indexing::prepared_grouped_soa_query_reader>
            grouped_soa_reader;
        std::vector<std::uint32_t> control_ordinals;
        std::vector<std::uint32_t> position_ordinals;
        std::vector<minimizer_generation_payload_read_group> control_groups;
        std::vector<minimizer_generation_payload_read_group> position_groups;
        std::vector<minimizer_generation_payload_read_group>
            selective_probe_control_groups;
        std::vector<minimizer_generation_payload_read_group>
            selective_probe_position_groups;
        bool selective_probe_full_generation = false;
    };
    auto* state = new fixture_read_state{};
    minimizer_generation_integration_fixture_result result{};
    try {
        Snapshot active_snapshot = bioseq::postgres::support::invoke_pg_as_cpp(
            [] { return GetActiveSnapshot(); });
        if (active_snapshot == nullptr || !IsMVCCSnapshot(active_snapshot)) {
            ready_minimizer_error_as_cpp(
                ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
                "bioseq minimizer generation fixture reader requires an active MVCC snapshot");
        }
        state->registered_snapshot = bioseq::postgres::support::invoke_pg_as_cpp(
            [active_snapshot] { return RegisterSnapshot(active_snapshot); });
        state->source_authorization.emplace(
            authorize_minimizer_generation_source_pg(
                identity, sequence_rowno, state->registered_snapshot));
        minimizer_generation_source_locator source =
            state->source_authorization->source_for_diagnostics();
        const std::uint64_t source_content_checksum =
            state->source_authorization->source_content_checksum_for_diagnostics();
        if (source.locator.logical_length == 0) {
            ready_minimizer_error_as_cpp(
                ERRCODE_INVALID_PARAMETER_VALUE,
                "bioseq minimizer generation fixture reader requires a nonempty target");
        }
        result.table_name = format_seqidx_name(
            index_name, rid.db_oid, rid.tablespace_oid, rid.relfilenode,
            seq_attno, source.locator.bucket_index, source.locator.shard_index);
        result.layout = layout;
        result.source_content_checksum = source_content_checksum;
        result.source_num_fragments = source.locator.fragment_count;
        result.source_total_bp = source.locator.logical_length;
        result.source_padding_letters = source.padding_letters;
        identity.table_name = result.table_name;

		const auto expected =
			bioseq::indexing::expected_grouped_soa_generation_identity_for_backend(
				layout,
				backend,
                source.alphabet_code,
                source.storage_unit_byte_width,
                kmer_size,
                window_size,
                bioseq::indexing::k_grouped_soa_default_max_occurrence_cap);
        if (!expected) {
            ready_minimizer_error_as_cpp(
                ERRCODE_INVALID_PARAMETER_VALUE,
                "bioseq minimizer generation fixture reader storage contract is invalid",
                fmt::format("wire_error={}",
                    static_cast<unsigned>(expected.error())));
        }
        state->session.emplace(
            result.table_name,
            sequence_rowno,
            state->registered_snapshot,
            *expected,
            rid.tablespace_oid,
            minimizer_generation_snapshot_registration::borrow_registered);
        index_storage_fragment_generation_read_session& session =
            *state->session;
        const minimizer_generation_head head = session.read_active_head_once();
        const minimizer_generation_read_metadata& metadata =
            session.read_manifest_directory_once(head);
        session.consume_full_source_authorization(
            std::move(*state->source_authorization));
        state->source_authorization.reset();

        result.generation_id = head.generation_id;
        result.storage_contract_checksum =
            bioseq::indexing::compute_minimizer_generation_storage_contract_checksum(
                metadata.manifest);
        source.storage_contract_checksum = result.storage_contract_checksum;
        result.source_identity = minimizer_generation_source_identity(rid.relfilenode, source);
        result.control_count = metadata.manifest.control_count;
        result.position_count = metadata.manifest.position_count;
        result.directory_entries = metadata.directory.entries().size();
        result.position_segment_count =
            metadata.manifest.position_segment_count;
        if (head.layout != layout
            || metadata.manifest.layout != layout
            || head.generation_id != metadata.manifest.generation_id
            || head.source_identity != result.source_identity
            || metadata.manifest.source_identity != result.source_identity
            || head.source_content_checksum != source_content_checksum
            || metadata.manifest.source_content_checksum
                != source_content_checksum
            || head.storage_contract_checksum
                != result.storage_contract_checksum) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "bioseq minimizer generation fixture reader identity disagrees");
        }

        /* Integration-only cold-read telemetry probe.  A directory entry is
         * an authenticated control+position pair, so a strict subset exists
         * only when the generation owns more than one populated entry.  Read
         * one exact entry in that case.  A one-entry generation is already the
         * complete payload and must use the full-generation scan; an empty
         * directory must execute the empty-payload stage protocol. This
         * keeps route identity, row counts, byte counts, and TID transitions
         * tied to I/O that actually occurred. */
        const auto directory_entries = metadata.directory.entries();
        const bool generation_payload_empty = directory_entries.empty();
        std::uint64_t total_directory_payload_rows = 0;
        for (const grouped_soa_bucket_directory_entry& entry : directory_entries) {
            total_directory_payload_rows =
                bioseq::common::saturating_add(
                    total_directory_payload_rows,
                    bioseq::common::saturating_add(
                        static_cast<std::uint64_t>(
                            entry.control_chunk_count),
                        static_cast<std::uint64_t>(
                            entry.position_chunk_count)));
        }
        if (total_directory_payload_rows
                == (std::numeric_limits<std::uint64_t>::max)()) {
            ready_minimizer_error_as_cpp(
                ERRCODE_PROGRAM_LIMIT_EXCEEDED,
                "bioseq minimizer generation fixture directory payload-row count overflows");
        }
        std::optional<std::size_t> selective_probe_entry;
        if (!generation_payload_empty) {
            for (std::size_t ordinal = 0;
                 ordinal < directory_entries.size(); ++ordinal) {
                const grouped_soa_bucket_directory_entry& entry =
                    directory_entries[ordinal];
                const std::uint64_t entry_payload_rows =
                    bioseq::common::saturating_add(
                        static_cast<std::uint64_t>(
                            entry.control_chunk_count),
                        static_cast<std::uint64_t>(
                            entry.position_chunk_count));
                if (entry.position_segment >= 0
                    && entry.control_chunk_count != 0
                    && entry.position_chunk_count != 0
                    && entry_payload_rows < total_directory_payload_rows
                    && entry.sub_key
                        != (std::numeric_limits<index_sub_key_t>::max)()) {
                    selective_probe_entry = ordinal;
                    break;
                }
            }
        }
        state->selective_probe_full_generation =
            !generation_payload_empty && !selective_probe_entry.has_value();
        if (selective_probe_entry) {
            const grouped_soa_bucket_directory_entry& entry =
                directory_entries[*selective_probe_entry];
            const auto position_sub_key =
                derive_minimizer_position_extent_sub_key(entry.sub_key);
            if (!position_sub_key
                || *position_sub_key
                    == (std::numeric_limits<index_sub_key_t>::max)()) {
                ready_minimizer_error_as_cpp(
                    ERRCODE_DATA_CORRUPTED,
                    "bioseq minimizer generation fixture selective position extent is invalid");
            }
            state->selective_probe_control_groups.push_back(
                minimizer_generation_payload_read_group{
                    .range = index_sub_key_range{
                        .generation_id = result.generation_id,
                        .position_segment = entry.position_segment,
                        .begin = entry.sub_key,
                        .end = entry.sub_key + 1,
                    },
                    .requests = {
                        index_sub_key_request{
                            .generation_id = result.generation_id,
                            .position_segment = entry.position_segment,
                            .sub_key = entry.sub_key,
                            .first_chunk_no = entry.control_first_chunk,
                            .expected_chunk_count = entry.control_chunk_count,
                            .expected_total_bytes = entry.control_encoded_bytes,
                            .expected_checksum = entry.control_checksum,
                        },
                    },
                });
            state->selective_probe_position_groups.push_back(
                minimizer_generation_payload_read_group{
                    .range = index_sub_key_range{
                        .generation_id = result.generation_id,
                        .position_segment = entry.position_segment,
                        .begin = *position_sub_key,
                        .end = *position_sub_key + 1,
                    },
                    .requests = {
                        index_sub_key_request{
                            .generation_id = result.generation_id,
                            .position_segment = entry.position_segment,
                            .sub_key = *position_sub_key,
                            .first_chunk_no = entry.position_first_chunk,
                            .expected_chunk_count = entry.position_chunk_count,
                            .expected_total_bytes = entry.position_encoded_bytes,
                            .expected_checksum = entry.position_checksum,
                        },
                    },
                });
        }

        const std::array<std::uint64_t, 1> query_hashes{
            generation_payload_empty
                ? std::uint64_t{1}
                : metadata.directory.entries().front().lo_hash};
        std::optional<minimizer_generation_encoding_error> reader_error;
        for (const grouped_soa_bucket_directory_entry& entry
             : metadata.directory.entries()) {
            if (entry.lo_hash <= query_hashes.front()
                && query_hashes.front() <= entry.hi_hash) {
                state->control_ordinals.push_back(entry.partition_id);
            }
        }
        state->position_ordinals = state->control_ordinals;
        auto reader = bioseq::indexing::prepared_grouped_soa_query_reader::begin(
            bioseq::parallelism::memory_admission_controller::instance(),
            head,
            metadata.manifest,
            *expected,
            metadata.directory.encoded_bytes(),
            query_hashes,
            metadata.manifest.max_occ_cap);
        if (reader) {
            state->grouped_soa_reader.emplace(std::move(*reader));
        } else {
            reader_error = reader.error();
        }
        if (reader_error) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "bioseq minimizer generation fixture reader could not prepare wire reader",
                fmt::format("layout={} wire_error={}",
                    static_cast<unsigned>(layout),
                    static_cast<unsigned>(*reader_error)));
        }
        if (!generation_payload_empty && state->control_ordinals.empty()) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "bioseq minimizer generation fixture reader selected no control extent");
        }

        std::uint64_t expected_control_bytes = 0;
        for (const std::uint32_t ordinal : state->control_ordinals) {
            const grouped_soa_bucket_directory_entry* entry =
                metadata.directory.entry_by_ordinal(ordinal);
            if (entry == nullptr || entry->sub_key < 0
                || entry->sub_key
                    == (std::numeric_limits<index_sub_key_t>::max)()) {
                ready_minimizer_error_as_cpp(
                    ERRCODE_DATA_CORRUPTED,
                    "bioseq minimizer generation fixture reader control ordinal is invalid");
            }
            expected_control_bytes = bioseq::common::saturating_add(
                expected_control_bytes, entry->control_encoded_bytes);
            result.control_encoded_bytes = bioseq::common::saturating_add(
                result.control_encoded_bytes, entry->control_encoded_bytes);
            result.control_chunks = bioseq::common::saturating_add(
                result.control_chunks, entry->control_chunk_count);
            state->control_groups.emplace_back();
            minimizer_generation_payload_read_group& group =
                state->control_groups.back();
            group.range = index_sub_key_range{
                .generation_id = result.generation_id,
                .position_segment = entry->position_segment,
                .begin = entry->sub_key,
                .end = entry->sub_key + 1,
            };
            group.requests.push_back(index_sub_key_request{
                .generation_id = result.generation_id,
                .position_segment = entry->position_segment,
                .sub_key = entry->sub_key,
                .first_chunk_no = entry->control_first_chunk,
                .expected_chunk_count = entry->control_chunk_count,
                .expected_total_bytes = entry->control_encoded_bytes,
                .expected_checksum = entry->control_checksum,
            });
        }
        if (expected_control_bytes
                == (std::numeric_limits<std::uint64_t>::max)()) {
            ready_minimizer_error_as_cpp(
                ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE,
                "bioseq minimizer generation fixture reader control bytes overflow");
        }
        session.plan_control_groups(state->control_groups);

        crc64_ecma182_accumulator control_crc{};
        crc64_ecma182_accumulator position_crc{};
        std::uint64_t control_bytes = 0;
        std::uint64_t position_bytes = 0;
        std::optional<minimizer_generation_encoding_error> direct_load_error;
        for (std::size_t group_index = 0;
             group_index < state->control_groups.size(); ++group_index) {
            (void)session.read_chunks_in_range(
                group_index,
                index_chunk_cache_policy::bypass_one_shot,
                [state, &control_crc, &control_bytes,
                 &direct_load_error](
                        const validated_minimizer_generation_row_receipt&
                            receipt) -> void {
                    const bool visited = bioseq::indexing::
                        visit_validated_minimizer_generation_row(
                            receipt,
                            [&control_crc, &control_bytes](
                                    const index_chunk_address&,
                                    std::span<const std::byte> payload,
                                    std::uint64_t) noexcept {
                                control_crc.update(payload);
                                control_bytes = bioseq::common::
                                    saturating_add(
                                        control_bytes, payload.size());
                            });
                    if (!visited) {
                        direct_load_error =
                            minimizer_generation_encoding_error::checksum;
                        return;
                    }
                    if (direct_load_error) {
                        return;
                    }
                    const auto accepted =
                        state->grouped_soa_reader->accept(receipt);
                    if (!accepted) {
                        direct_load_error = accepted.error();
                    }
                });
        }
        if (direct_load_error || control_bytes != expected_control_bytes) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "bioseq minimizer generation fixture reader control load failed",
                fmt::format(
                    "wire_error={} actual_bytes={} expected_bytes={}",
                    direct_load_error
                        ? static_cast<unsigned>(*direct_load_error) + 1u : 0u,
                    control_bytes, expected_control_bytes));
        }

        if (!generation_payload_empty && state->position_ordinals.empty()) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "bioseq minimizer generation fixture reader selected no position extent");
        }

        std::uint64_t expected_position_bytes = 0;
        for (const std::uint32_t ordinal : state->position_ordinals) {
            const grouped_soa_bucket_directory_entry* entry =
                metadata.directory.entry_by_ordinal(ordinal);
            const auto position_sub_key = entry == nullptr
                ? std::expected<index_sub_key_t, minimizer_generation_encoding_error>{
                    std::unexpected(minimizer_generation_encoding_error::partition_id)}
                : derive_minimizer_position_extent_sub_key(entry->sub_key);
            if (entry == nullptr || !position_sub_key
                || *position_sub_key
                    == (std::numeric_limits<index_sub_key_t>::max)()) {
                ready_minimizer_error_as_cpp(
                    ERRCODE_DATA_CORRUPTED,
                    "bioseq minimizer generation fixture reader position ordinal is invalid");
            }
            expected_position_bytes = bioseq::common::saturating_add(
                expected_position_bytes, entry->position_encoded_bytes);
            result.position_encoded_bytes = bioseq::common::saturating_add(
                result.position_encoded_bytes, entry->position_encoded_bytes);
            result.position_chunks = bioseq::common::saturating_add(
                result.position_chunks, entry->position_chunk_count);
            state->position_groups.emplace_back();
            minimizer_generation_payload_read_group& group =
                state->position_groups.back();
            group.range = index_sub_key_range{
                .generation_id = result.generation_id,
                .position_segment = entry->position_segment,
                .begin = *position_sub_key,
                .end = *position_sub_key + 1,
            };
            group.requests.push_back(index_sub_key_request{
                .generation_id = result.generation_id,
                .position_segment = entry->position_segment,
                .sub_key = *position_sub_key,
                .first_chunk_no = entry->position_first_chunk,
                .expected_chunk_count = entry->position_chunk_count,
                .expected_total_bytes = entry->position_encoded_bytes,
                .expected_checksum = entry->position_checksum,
            });
        }
        if (expected_position_bytes
                == (std::numeric_limits<std::uint64_t>::max)()) {
            ready_minimizer_error_as_cpp(
                ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE,
                "bioseq minimizer generation fixture reader position bytes overflow");
        }
        session.plan_position_groups(state->position_groups);
        for (std::size_t group_index = 0;
             group_index < state->position_groups.size(); ++group_index) {
            (void)session.read_chunks_in_range(
                group_index,
                index_chunk_cache_policy::bypass_one_shot,
                [state, &position_crc, &position_bytes,
                 &direct_load_error](
                        const validated_minimizer_generation_row_receipt&
                            receipt) -> void {
                    const bool visited = bioseq::indexing::
                        visit_validated_minimizer_generation_row(
                            receipt,
                            [&position_crc, &position_bytes](
                                    const index_chunk_address&,
                                    std::span<const std::byte> payload,
                                    std::uint64_t) noexcept {
                                position_crc.update(payload);
                                position_bytes = bioseq::common::
                                    saturating_add(
                                        position_bytes, payload.size());
                            });
                    if (!visited) {
                        direct_load_error =
                            minimizer_generation_encoding_error::checksum;
                        return;
                    }
                    if (direct_load_error) {
                        return;
                    }
                    const auto accepted =
                        state->grouped_soa_reader->accept(receipt);
                    if (!accepted) {
                        direct_load_error = accepted.error();
                    }
                });
        }
        if (direct_load_error || position_bytes != expected_position_bytes) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "bioseq minimizer generation fixture reader position load failed",
                fmt::format(
                    "wire_error={} actual_bytes={} expected_bytes={}",
                    direct_load_error
                        ? static_cast<unsigned>(*direct_load_error) + 1u : 0u,
                    position_bytes, expected_position_bytes));
        }

        const auto finished = state->grouped_soa_reader->finish();
        if (!finished) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "bioseq grouped-SoA fixture reader final validation failed",
                fmt::format("wire_error={}",
                    static_cast<unsigned>(finished.error())));
        }
        const auto lookup =
            state->grouped_soa_reader->lookup(query_hashes.front());
        if ((!generation_payload_empty
                && (lookup.global_occurrence_count == 0
                    || !lookup.positions_complete
                    || lookup.positions.size()
                        != lookup.global_occurrence_count
                || std::ranges::any_of(
                    lookup.positions,
                    [&source](const std::uint64_t encoded_position) {
                        return (encoded_position >> 1u)
                            >= source.locator.logical_length;
                    })))
            || (generation_payload_empty
                && (lookup.global_occurrence_count != 0
                    || !lookup.positions.empty()
                    || !lookup.positions_complete))) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "bioseq grouped-SoA fixture lookup failed",
                fmt::format("count={} positions={}",
                    lookup.global_occurrence_count,
                    lookup.positions.size()));
        }
        result.lookup_count = lookup.global_occurrence_count;
        result.lookup_bp = lookup.positions.empty()
            ? 0u : lookup.positions.front() >> 1u;
        result.lookup_reverse = lookup.positions.empty()
            ? false
            : (lookup.positions.front() & std::uint64_t{1}) != 0u;
        result.positions_complete = lookup.positions_complete;
        if ((!generation_payload_empty
                && (result.lookup_count == 0
                    || result.lookup_bp >= source.locator.logical_length
                    || !result.positions_complete))
            || (generation_payload_empty
                && (result.lookup_count != 0
                    || result.lookup_bp != 0
                    || result.lookup_reverse
                    || !result.positions_complete
                    || metadata.manifest.control_count != 0
                    || metadata.manifest.position_count != 0
                    || metadata.manifest.payload_bytes != 0))) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "bioseq minimizer generation fixture reader lookup validation disagrees");
        }

        result.validated_control_bytes = control_bytes;
        result.validated_position_bytes = position_bytes;
        result.validated_control_checksum = control_crc.checksum();
        result.validated_position_checksum = position_crc.checksum();
        result.read_stats = session.finish();
        if (result.read_stats.controls.plan_calls != 1
            || result.read_stats.positions.plan_calls != 1
            || result.read_stats.controls.planned_request_groups
                != state->control_groups.size()
            || result.read_stats.positions.planned_request_groups
                != state->position_groups.size()
            || result.read_stats.controls.executed_request_groups
                != state->control_groups.size()
            || result.read_stats.positions.executed_request_groups
                != state->position_groups.size()
            || result.read_stats.positions.control_authorization_checks
                != state->position_groups.size()
            || result.read_stats.payload_bytes_read
                != control_bytes + position_bytes) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "bioseq minimizer generation fixture reader two-stage validation disagrees");
        }

        state->grouped_soa_reader.reset();
        state->session.reset();

        if (state->selective_probe_full_generation) {
            state->catalog_source_authorization.emplace(
                authorize_minimizer_generation_catalog_source_pg(
                    identity, sequence_rowno, state->registered_snapshot));
        } else {
            state->source_authorization.emplace(
                authorize_minimizer_generation_source_pg(
                    identity, sequence_rowno, state->registered_snapshot));
        }
        state->session.emplace(
            result.table_name,
            sequence_rowno,
            state->registered_snapshot,
            *expected,
            rid.tablespace_oid,
            minimizer_generation_snapshot_registration::borrow_registered);
        index_storage_fragment_generation_read_session& probe_session =
            *state->session;
        const minimizer_generation_head probe_head =
            probe_session.read_active_head_once();
        const minimizer_generation_read_metadata& probe_metadata =
            probe_session.read_manifest_directory_once(probe_head);
        if (state->selective_probe_full_generation) {
            probe_session.consume_source_authorization(
                std::move(*state->catalog_source_authorization));
            state->catalog_source_authorization.reset();
        } else {
            probe_session.consume_full_source_authorization(
                std::move(*state->source_authorization));
            state->source_authorization.reset();
        }
        if (state->selective_probe_full_generation) {
            (void)probe_session.read_full_generation_payload_once(
                [](const validated_minimizer_generation_row_receipt&) {},
                [](const validated_minimizer_generation_row_receipt&) {});
        } else {
            probe_session.plan_control_groups(
                state->selective_probe_control_groups);
            if (!state->selective_probe_control_groups.empty()) {
                (void)probe_session.read_chunks_in_range(
                    0, index_chunk_cache_policy::bypass_one_shot,
                    [](const index_chunk_address&, const char*, std::size_t) {});
            }
            probe_session.plan_position_groups(
                state->selective_probe_position_groups);
            if (!state->selective_probe_position_groups.empty()) {
                (void)probe_session.read_chunks_in_range(
                    0, index_chunk_cache_policy::bypass_one_shot,
                    [](const index_chunk_address&, const char*, std::size_t) {});
            }
        }
        result.selective_probe_stats = probe_session.finish();
        const cold_index_preparation_stats& probe_cold =
            result.selective_probe_stats.cold;
        std::uint64_t probe_tid_transitions = 0;
        probe_tid_transitions = bioseq::common::saturating_add(
            probe_tid_transitions, probe_cold.heap_tid_adjacent);
        probe_tid_transitions = bioseq::common::saturating_add(
            probe_tid_transitions, probe_cold.heap_tid_forward_gap);
        probe_tid_transitions = bioseq::common::saturating_add(
            probe_tid_transitions, probe_cold.heap_tid_backward_jump);
        const std::uint64_t expected_scanned_rows =
            bioseq::common::saturating_add(
                probe_cold.payload_rows_delivered,
                probe_cold.payload_rows_skipped);
        const bool expected_empty_route = generation_payload_empty;
        const bool expected_full_route =
            state->selective_probe_full_generation;
        const bool expected_selective_route =
            !expected_empty_route && !expected_full_route;
        const bool payload_equations_disagree = expected_empty_route
            ? probe_cold.payload_rows_requested != 0
                || probe_cold.payload_rows_delivered != 0
                || probe_cold.payload_rows_scanned != 0
                || probe_cold.payload_rows_skipped != 0
                || probe_cold.payload_bytes_requested != 0
                || probe_cold.payload_bytes_delivered != 0
                || probe_cold.payload_bytes_scanned != 0
                || probe_cold.row_crc_bytes != 0
                || probe_cold.extent_crc_bytes != 0
                || probe_cold.extent_crc_raw_bytes_hashed != 0
                || probe_cold.extent_crc_combined_bytes != 0
                || probe_cold.extent_crc_combine_rows != 0
                || probe_tid_transitions != 0
            : probe_cold.payload_rows_requested
                    != probe_cold.payload_rows_delivered
                || probe_cold.payload_rows_delivered == 0
                || probe_cold.payload_rows_skipped != 0
                || probe_cold.payload_rows_scanned != expected_scanned_rows
                || probe_cold.payload_bytes_requested
                    != probe_cold.payload_bytes_delivered
                || probe_cold.payload_bytes_delivered == 0
                || probe_cold.payload_bytes_scanned
                    != probe_cold.payload_bytes_delivered
                || probe_cold.row_crc_bytes
                    != probe_cold.payload_bytes_scanned
                || probe_cold.extent_crc_bytes
                    != probe_cold.payload_bytes_delivered
                || probe_cold.extent_crc_raw_bytes_hashed != 0
                || probe_cold.extent_crc_combined_bytes
                    != probe_cold.payload_bytes_delivered
                || probe_cold.extent_crc_combine_rows
                    != probe_cold.payload_rows_delivered
                || probe_tid_transitions
                    != probe_cold.payload_rows_scanned - 1u;
        if (probe_cold.route_empty
                != static_cast<std::uint64_t>(expected_empty_route)
            || probe_cold.route_selective
                != static_cast<std::uint64_t>(expected_selective_route)
            || probe_cold.route_full
                != static_cast<std::uint64_t>(expected_full_route)
            || payload_equations_disagree
            || expected_scanned_rows
                == (std::numeric_limits<std::uint64_t>::max)()
            || probe_tid_transitions
                == (std::numeric_limits<std::uint64_t>::max)()
            || probe_cold.decode_ns != 0
            || probe_cold.transfer_ns != 0) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "bioseq minimizer generation fixture cold stats equations disagree");
        }
        state->session.reset();
        bioseq::postgres::support::invoke_pg_as_cpp(
            [state] { UnregisterSnapshot(state->registered_snapshot); });
        state->registered_snapshot = nullptr;
        delete state;
        state = nullptr;
    } catch (...) {
        std::exception_ptr error = std::current_exception();
        if (state != nullptr) {
            state->grouped_soa_reader.reset();
            state->session.reset();
            state->source_authorization.reset();
            state->catalog_source_authorization.reset();
        }
        if (state != nullptr && state->registered_snapshot != nullptr) {
            Snapshot cleanup = std::exchange(
                state->registered_snapshot, nullptr);
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

/// Reopen a READY grouped-SoA generation and stream its complete opaque payload
/// through the generation-wide single-scan API. This seam deliberately does
/// not decode either payload and exists only for integration validation.
[[nodiscard]] inline minimizer_generation_full_payload_fixture_result
read_minimizer_generation_full_payload_fixture_impl(
        Oid rel_oid,
        std::string_view index_name,
        std::uint64_t sequence_rowno,
        std::uint16_t kmer_size,
        std::uint16_t window_size,
        bioseq::indexing::minimizer_generation_layout layout,
        bioseq::minimizers::minimizer_sketch_backend backend) {
    if (!OidIsValid(rel_oid)
        || !bioseq::postgres::support::invoke_pg_as_cpp(
            [rel_oid] { return relation_oid_uses_column_table_access_method(rel_oid); })) {
        ready_minimizer_error_as_cpp(
            ERRCODE_WRONG_OBJECT_TYPE,
            "bioseq full-generation reader requires a USING bioseq table");
    }
    if (!bioseq::postgres::support::invoke_pg_as_cpp([] { return superuser(); })) {
        ready_minimizer_error_as_cpp(
            ERRCODE_INSUFFICIENT_PRIVILEGE,
            "bioseq full-generation reader requires superuser");
    }
    if (index_name != k_minimizers_per_fragment_index_name
        && index_name != k_minimizers_per_sequence_index_name) {
        ready_minimizer_error_as_cpp(
            ERRCODE_INVALID_PARAMETER_VALUE,
            "bioseq full-generation reader requires a minimizer index");
    }
    if (kmer_size == 0 || window_size == 0
        || !bioseq::indexing::sql_bigint_can_represent(sequence_rowno)
        || !bioseq::indexing::is_supported_minimizer_generation_layout(
            layout)) {
        ready_minimizer_error_as_cpp(
            ERRCODE_INVALID_PARAMETER_VALUE,
            "bioseq full-generation reader parameters are invalid");
    }

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
    struct read_state {
        Snapshot registered_snapshot = nullptr;
        std::optional<minimizer_generation_catalog_source_authorization> source_authorization;
        std::optional<index_storage_fragment_generation_read_session> session;
    };
    read_state* state = nullptr;
    minimizer_generation_full_payload_fixture_result result{};
    try {
        state = new read_state{};
        Snapshot active_snapshot = bioseq::postgres::support::invoke_pg_as_cpp(
            [] { return GetActiveSnapshot(); });
        if (active_snapshot == nullptr || !IsMVCCSnapshot(active_snapshot)) {
            ready_minimizer_error_as_cpp(
                ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
                "bioseq full-generation reader requires an active MVCC snapshot");
        }
        state->registered_snapshot = bioseq::postgres::support::invoke_pg_as_cpp(
            [active_snapshot] { return RegisterSnapshot(active_snapshot); });
        state->source_authorization.emplace(
            authorize_minimizer_generation_catalog_source_pg(
                identity, sequence_rowno, state->registered_snapshot));
        const minimizer_generation_source_locator source =
            state->source_authorization->source_for_diagnostics();
        result.table_name = format_seqidx_name(
            index_name, rid.db_oid, rid.tablespace_oid, rid.relfilenode,
            seq_attno, source.locator.bucket_index, source.locator.shard_index);
        result.selected_backend_name =
            bioseq::minimizers::minimizer_sketch_backend_name(
                backend);
        identity.table_name = result.table_name;
        const auto expected =
            bioseq::indexing::expected_grouped_soa_generation_identity_for_backend(
                layout,
                backend,
                source.alphabet_code,
                source.storage_unit_byte_width,
                kmer_size,
                window_size,
                layout == bioseq::indexing::minimizer_generation_layout::
                        grouped_soa_segment_local
                    ? std::optional<std::uint64_t>{bioseq::indexing::
                        k_grouped_soa_default_max_occurrence_cap}
                    : std::nullopt);
        if (!expected) {
            ready_minimizer_error_as_cpp(
                ERRCODE_INVALID_PARAMETER_VALUE,
                "bioseq full-generation storage contract is invalid",
                fmt::format("wire_error={}",
                    static_cast<unsigned>(expected.error())));
        }
        state->session.emplace(
            result.table_name, sequence_rowno, state->registered_snapshot,
            *expected, rid.tablespace_oid,
            minimizer_generation_snapshot_registration::borrow_registered);
        index_storage_fragment_generation_read_session& session =
            *state->session;
        const minimizer_generation_head head = session.read_active_head_once();
        const minimizer_generation_read_metadata& metadata =
            session.read_manifest_directory_once(head);
        const minimizer_generation_catalog_source_stats source_stats =
            state->source_authorization->stats_for_diagnostics();
        session.consume_source_authorization(
            std::move(*state->source_authorization));
        state->source_authorization.reset();
        const auto requested_scheme =
            bioseq::indexing::minimizer_scheme_identity_for_backend(backend);
        const auto requested_producer_backend = bioseq::indexing::
            minimizer_producer_backend_identity_for_backend(backend);
        if (metadata.manifest.minimizer_scheme_id != requested_scheme.id
            || metadata.manifest.minimizer_scheme_revision
                != requested_scheme.revision
            || metadata.manifest.producer_backend_id
                != requested_producer_backend.id
            || metadata.manifest.producer_identity_revision
                != requested_producer_backend.revision
            || source_stats.parent_rows_read != 1
            || source_stats.catalog_rows_read != 1
            || source_stats.catalog_index_scans != 1
            || source_stats.fragment_rows_read != 0
            || source_stats.decoded_bp != 0) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "bioseq full-generation backend/source authorization disagrees");
        }
        result.generation_id = metadata.manifest.generation_id;
        crc64_ecma182_accumulator control_checksum{};
        crc64_ecma182_accumulator position_checksum{};
        const index_chunk_read_stats payload =
            session.read_full_generation_payload_once(
                [&result, &control_checksum](
                        const index_chunk_address&,
                        const char* bytes,
                        std::size_t byte_count) -> void {
                    control_checksum.update(std::as_bytes(
                        std::span<const char>{bytes, byte_count}));
                    result.control_bytes =
                        bioseq::common::saturating_add(
                            result.control_bytes, byte_count);
                },
                [&result, &position_checksum](
                        const index_chunk_address&,
                        const char* bytes,
                        std::size_t byte_count) -> void {
                    position_checksum.update(std::as_bytes(
                        std::span<const char>{bytes, byte_count}));
                    result.position_bytes =
                        bioseq::common::saturating_add(
                            result.position_bytes, byte_count);
                });
        result.control_checksum = control_checksum.checksum();
        result.position_checksum = position_checksum.checksum();
        result.read_stats = session.finish();
        if (result.control_bytes
                != result.read_stats.full_generation.control_bytes
            || result.position_bytes
                != result.read_stats.full_generation.position_bytes
            || payload.index_scans != 1
            || result.read_stats.full_generation.scan_calls != 1
            || result.read_stats.range_scan_calls != 1
            || result.read_stats.index_scans != 3
            || result.read_stats.source_parent_rows_read != 1
            || result.read_stats.source_catalog_rows_read != 1
            || result.read_stats.source_catalog_index_scans != 1
            || result.read_stats.source_fragment_rows_read != 0
            || result.read_stats.source_decoded_bp != 0
            || result.read_stats.payload.delivered_bytes
                != result.control_bytes + result.position_bytes) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "bioseq full-generation single-scan validation disagrees");
        }
        state->session.reset();
        bioseq::postgres::support::invoke_pg_as_cpp(
            [state] { UnregisterSnapshot(state->registered_snapshot); });
        state->registered_snapshot = nullptr;
        delete state;
        state = nullptr;
    } catch (...) {
        std::exception_ptr error = std::current_exception();
        if (state != nullptr) {
            state->session.reset();
            state->source_authorization.reset();
        }
        if (state != nullptr && state->registered_snapshot != nullptr) {
            Snapshot cleanup = std::exchange(
                state->registered_snapshot, nullptr);
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

/// Integration-only source adversary: remove one referenced physical sequence
/// fragment by its locator ordinal. The public source authorization must subsequently
/// fail while walking the same locator under its registered snapshot.  Ordinal
/// zero is deliberately forbidden so this seam validates later-fragment
/// participation rather than merely checking the first physical row.
inline void remove_sequence_fragment_impl(
        Oid rel_oid,
        std::string_view index_name,
        std::uint64_t sequence_rowno,
        std::uint32_t fragment_ordinal) {
    if (!bioseq::postgres::support::invoke_pg_as_cpp([] { return superuser(); })) {
        ready_minimizer_error_as_cpp(
            ERRCODE_INSUFFICIENT_PRIVILEGE,
            "bioseq source-fragment adversary requires superuser");
    }
    if (!OidIsValid(rel_oid)
        || !bioseq::postgres::support::invoke_pg_as_cpp(
            [rel_oid] { return relation_oid_uses_column_table_access_method(rel_oid); })) {
        ready_minimizer_error_as_cpp(
            ERRCODE_WRONG_OBJECT_TYPE,
            "bioseq source-fragment adversary requires a USING bioseq table");
    }
    if (index_name != k_minimizers_per_fragment_index_name
        && index_name != k_minimizers_per_sequence_index_name) {
        ready_minimizer_error_as_cpp(
            ERRCODE_INVALID_PARAMETER_VALUE,
            "bioseq source-fragment adversary requires a minimizer index");
    }
    if (fragment_ordinal == 0
        || !bioseq::indexing::sql_bigint_can_represent(sequence_rowno)) {
        ready_minimizer_error_as_cpp(
            ERRCODE_INVALID_PARAMETER_VALUE,
            "bioseq source-fragment adversary requires a non-first fragment");
    }

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

    struct remove_state {
        Snapshot registered_snapshot = nullptr;
        seq_caches* fragment_caches = nullptr;
    };
    auto* state = new remove_state{};
    try {
        Snapshot active_snapshot = bioseq::postgres::support::invoke_pg_as_cpp(
            [] { return GetActiveSnapshot(); });
        if (active_snapshot == nullptr || !IsMVCCSnapshot(active_snapshot)) {
            ready_minimizer_error_as_cpp(
                ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
                "bioseq source-fragment adversary requires an active MVCC snapshot");
        }
        state->registered_snapshot = bioseq::postgres::support::invoke_pg_as_cpp(
            [active_snapshot] { return RegisterSnapshot(active_snapshot); });
        minimizer_generation_source_authorization authorization =
            authorize_minimizer_generation_source_pg(
                identity, sequence_rowno, state->registered_snapshot);
        const minimizer_generation_source_locator source =
            authorization.source_for_diagnostics();
        if (source.locator.fragment_count < 3
            || fragment_ordinal >= source.locator.fragment_count) {
            ready_minimizer_error_as_cpp(
                ERRCODE_INVALID_PARAMETER_VALUE,
                "bioseq source-fragment adversary ordinal is out of range",
                fmt::format("ordinal={} fragments={}", fragment_ordinal,
                    source.locator.fragment_count));
        }

        state->fragment_caches = bioseq::postgres::support::invoke_pg_as_cpp(
            [] { return alloc_seq_caches(CurrentMemoryContext); });
        state->fragment_caches->side = seq_caches::access_side::read;
        state->fragment_caches->lock_mode = RowExclusiveLock;
        state->fragment_caches->main_database_oid = rid.db_oid;
        state->fragment_caches->main_tablespace_oid = rid.tablespace_oid;
        state->fragment_caches->main_relfilenode = rid.relfilenode;
        [[maybe_unused]] Relation fragment_relation =
            bioseq::postgres::support::invoke_pg_as_cpp(
                [rid, seq_attno, &source, state] {
                return open_sub_table_for_locator(
                    rid.db_oid, rid.tablespace_oid, rid.relfilenode, seq_attno,
                    source.locator, RowExclusiveLock,
                    state->fragment_caches);
            });
        const std::uint64_t fragment_rowno =
            source.locator.first_fragment_row + fragment_ordinal;
        if (fragment_rowno < source.locator.first_fragment_row) {
            ready_minimizer_error_as_cpp(
                ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE,
                "bioseq source-fragment row number overflow");
        }
        // Registered source fragments are immutable. Exercise the supported
        // mutation path by transactionally invalidating the exact epoch rather
        // than bypassing the TableAM guard and creating undetectable raw damage.
        bioseq::postgres::support::invoke_pg_as_cpp([&source] {
            sequence_source_version_manager::invalidate_epoch(
                source.locator.source_epoch);
        });
        const TM_Result deleted = TM_Ok;
        if (deleted != TM_Ok) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "bioseq source-fragment adversary could not remove the referenced row",
                fmt::format("ordinal={} tm_result={}", fragment_ordinal,
                    static_cast<int>(deleted)));
        }
        bioseq::postgres::support::invoke_pg_as_cpp([state] {
            close_and_destroy_seq_caches(
                state->fragment_caches, /*commit=*/true);
        });
        state->fragment_caches = nullptr;
        bioseq::postgres::support::invoke_pg_as_cpp(
            [state] { UnregisterSnapshot(state->registered_snapshot); });
        state->registered_snapshot = nullptr;
        bioseq::postgres::support::invoke_pg_as_cpp([] {
            CommandCounterIncrement();
            UpdateActiveSnapshotCommandId();
        });
        delete state;
        state = nullptr;
    } catch (...) {
        std::exception_ptr error = std::current_exception();
        if (state != nullptr && state->fragment_caches != nullptr) {
            seq_caches* cleanup = std::exchange(
                state->fragment_caches, nullptr);
            try {
                bioseq::postgres::support::invoke_pg_as_cpp([cleanup] {
                    close_and_destroy_seq_caches(
                        cleanup, /*commit=*/false);
                });
            } catch (...) {
                error = std::current_exception();
            }
        }
        if (state != nullptr && state->registered_snapshot != nullptr) {
            Snapshot cleanup = std::exchange(
                state->registered_snapshot, nullptr);
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
}

/// Exercise read-session failure transitions against the live fixture while
/// restoring the backend memory cap and registered snapshot on every ERROR.
inline void exercise_minimizer_generation_session_failure_impl(
        Oid rel_oid,
        std::string_view index_name,
        std::uint64_t sequence_rowno,
        std::string_view mode) {
    if (!bioseq::postgres::support::invoke_pg_as_cpp([] { return superuser(); })) {
        ready_minimizer_error_as_cpp(
            ERRCODE_INSUFFICIENT_PRIVILEGE,
            "bioseq minimizer session fixture requires superuser");
    }
    if (mode != "repeated_head" && mode != "repeated_metadata"
        && mode != "early_payload" && mode != "permit_denied"
        && mode != "scheme_mismatch"
        && mode != "layout_mismatch"
        && mode != "position_before_controls"
        && mode != "finish_before_positions"
        && mode != "repeated_control_plan"
        && mode != "control_position_namespace"
        && mode != "unauthorized_position") {
        ready_minimizer_error_as_cpp(
            ERRCODE_INVALID_PARAMETER_VALUE,
            "bioseq minimizer session fixture mode is invalid");
    }
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
    struct failure_state {
        Snapshot registered_snapshot = nullptr;
        std::uint64_t saved_memory_cap = 0;
        bool cap_changed = false;
        std::optional<minimizer_generation_source_authorization> source_authorization;
        std::optional<index_storage_fragment_generation_read_session>
            read_session;
        std::vector<minimizer_generation_payload_read_group> groups;
    };
    auto* state = new failure_state{};
    try {
        Snapshot active_snapshot = bioseq::postgres::support::invoke_pg_as_cpp(
            [] { return GetActiveSnapshot(); });
        if (active_snapshot == nullptr || !IsMVCCSnapshot(active_snapshot)) {
            ready_minimizer_error_as_cpp(
                ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
                "bioseq minimizer session fixture requires an active MVCC snapshot");
        }
        state->registered_snapshot = bioseq::postgres::support::invoke_pg_as_cpp(
            [active_snapshot] { return RegisterSnapshot(active_snapshot); });
        state->source_authorization.emplace(
            authorize_minimizer_generation_source_pg(
                identity, sequence_rowno, state->registered_snapshot));
        const minimizer_generation_source_locator& source =
            state->source_authorization->source_for_diagnostics();
        identity.table_name = format_seqidx_name(
            index_name, rid.db_oid, rid.tablespace_oid, rid.relfilenode,
            seq_attno, source.locator.bucket_index, source.locator.shard_index);
        if (mode == "permit_denied") {
            auto& admission =
                bioseq::parallelism::memory_admission_controller::instance();
            state->saved_memory_cap = admission.index_memory_cap_bytes();
            admission.set_index_memory_cap_bytes(1);
            state->cap_changed = true;
        }
        auto expected =
            bioseq::indexing::expected_grouped_soa_generation_identity_for_backend(
                bioseq::indexing::minimizer_generation_layout::
                    grouped_soa_segment_local,
                bioseq::minimizers::minimizer_sketch_backend::scalar,
                source.alphabet_code,
                source.storage_unit_byte_width,
                static_cast<std::uint16_t>(bioseq::aligner::settings::minimizer::kmer_size),
                static_cast<std::uint16_t>(bioseq::aligner::settings::minimizer::window_size),
                bioseq::indexing::k_grouped_soa_default_max_occurrence_cap);
        if (!expected) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "bioseq minimizer session expected storage contract is invalid");
        }
        if (mode == "scheme_mismatch") {
            expected->minimizer_scheme_id =
                bioseq::indexing::k_bounded_nthash_u32_r7_sum_scheme_id;
            expected->minimizer_scheme_revision =
                bioseq::indexing::k_bounded_nthash_u32_r7_sum_scheme_revision;
        } else if (mode == "layout_mismatch") {
            expected->layout =
                bioseq::indexing::minimizer_generation_layout::invalid;
        }
        state->read_session.emplace(
            identity.table_name, sequence_rowno, state->registered_snapshot,
            *expected,
            rid.tablespace_oid,
            minimizer_generation_snapshot_registration::borrow_registered);
        index_storage_fragment_generation_read_session& session =
            *state->read_session;
        const minimizer_generation_head head = session.read_active_head_once();
        if (mode == "repeated_head") {
            (void)session.read_active_head_once();
        } else if (mode == "early_payload") {
            session.plan_control_groups(
                std::span<const minimizer_generation_payload_read_group>{});
        } else {
            const minimizer_generation_read_metadata& metadata =
                session.read_manifest_directory_once(head);
            if (mode == "repeated_metadata") {
                (void)session.read_manifest_directory_once(head);
            } else if (mode == "position_before_controls") {
                session.consume_full_source_authorization(
                    std::move(*state->source_authorization));
                state->source_authorization.reset();
                session.plan_position_groups(
                    std::span<const minimizer_generation_payload_read_group>{});
            } else if (mode == "finish_before_positions") {
                session.consume_full_source_authorization(
                    std::move(*state->source_authorization));
                state->source_authorization.reset();
                session.plan_control_groups(
                    std::span<const minimizer_generation_payload_read_group>{});
                (void)session.finish();
            } else if (mode == "repeated_control_plan") {
                session.consume_full_source_authorization(
                    std::move(*state->source_authorization));
                state->source_authorization.reset();
                session.plan_control_groups(
                    std::span<const minimizer_generation_payload_read_group>{});
                session.plan_control_groups(
                    std::span<const minimizer_generation_payload_read_group>{});
            } else if (mode == "control_position_namespace"
                       || mode == "unauthorized_position") {
                session.consume_full_source_authorization(
                    std::move(*state->source_authorization));
                state->source_authorization.reset();
                const auto entries = metadata.directory.entries();
                if (entries.empty()) {
                    ready_minimizer_error_as_cpp(
                        ERRCODE_DATA_CORRUPTED,
                        "bioseq minimizer session fixture has no payload entry");
                }
                const grouped_soa_bucket_directory_entry& entry = entries.front();
                const auto position_sub_key =
                    derive_minimizer_position_extent_sub_key(entry.sub_key);
                if (!position_sub_key) {
                    ready_minimizer_error_as_cpp(
                        ERRCODE_DATA_CORRUPTED,
                        "bioseq minimizer session fixture position key is invalid");
                }
                state->groups.push_back(minimizer_generation_payload_read_group{
                    .range = index_sub_key_range{
                        .generation_id = head.generation_id,
                        .position_segment = entry.position_segment,
                        .begin = *position_sub_key,
                        .end = *position_sub_key + 1,
                    },
                    .requests = {
                        index_sub_key_request{
                            .generation_id = head.generation_id,
                            .position_segment = entry.position_segment,
                            .sub_key = *position_sub_key,
                            .first_chunk_no = entry.position_first_chunk,
                            .expected_chunk_count = entry.position_chunk_count,
                            .expected_total_bytes = entry.position_encoded_bytes,
                            .expected_checksum = entry.position_checksum,
                        },
                    },
                });
                if (mode == "control_position_namespace") {
                    session.plan_control_groups(
                        std::span<const minimizer_generation_payload_read_group>{
                            state->groups});
                } else {
                    session.plan_control_groups(
                        std::span<const minimizer_generation_payload_read_group>{});
                    session.plan_position_groups(
                        std::span<const minimizer_generation_payload_read_group>{
                            state->groups});
                }
            }
        }
        if (state->cap_changed) {
            bioseq::parallelism::memory_admission_controller::instance()
                .set_index_memory_cap_bytes(state->saved_memory_cap);
            state->cap_changed = false;
        }
        state->source_authorization.reset();
        state->read_session.reset();
        bioseq::postgres::support::invoke_pg_as_cpp(
            [state] { UnregisterSnapshot(state->registered_snapshot); });
        state->registered_snapshot = nullptr;
        ready_minimizer_error_as_cpp(
            ERRCODE_INTERNAL_ERROR,
            "bioseq minimizer session fixture did not reach its expected failure");
    } catch (...) {
        std::exception_ptr error = std::current_exception();
        if (state->cap_changed) {
            bioseq::parallelism::memory_admission_controller::instance()
                .set_index_memory_cap_bytes(state->saved_memory_cap);
            state->cap_changed = false;
        }
        state->read_session.reset();
        state->source_authorization.reset();
        if (state->registered_snapshot != nullptr) {
            Snapshot cleanup = std::exchange(
                state->registered_snapshot, nullptr);
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
}

namespace minimizer_generation_storage_test_detail {

/// Chunk and assembled-cache keys addressing one fixture generation.
struct cache_keys {
    index_chunk_key chunk{};
    assembled_index_key assembled{};
};

/// Derive fixture cache keys from one physical source identity.
[[nodiscard]] inline cache_keys make_cache_keys(
        Oid user_relid,
        const relation_identity& relation_identity,
        std::int16_t column_attno,
        std::string_view index_name,
        const minimizer_generation_source_locator& source,
        std::uint64_t sequence_rowno) {
    const std::uint64_t name_hash = index_name_hash(
        relation_identity.db_oid, relation_identity.tablespace_oid,
        relation_identity.relfilenode, column_attno, index_name);
    return cache_keys{
        .chunk = index_chunk_key{
            .db_oid = relation_identity.db_oid,
            .user_relid = user_relid,
            .tablespace_oid = relation_identity.tablespace_oid,
            .main_relfilenode = relation_identity.relfilenode,
            .column_attno = column_attno,
            .index_name_hash = name_hash,
            .bucket_idx = static_cast<std::uint8_t>(
                source.locator.bucket_index),
            .shard_idx = source.locator.shard_index,
            .query_key = sequence_rowno,
            .sub_key = 0,
            .chunk_no = 0,
        },
        .assembled = assembled_index_key{
            .db_oid = relation_identity.db_oid,
            .user_relid = user_relid,
            .tablespace_oid = relation_identity.tablespace_oid,
            .main_relfilenode = relation_identity.relfilenode,
            .column_attno = column_attno,
            .index_name_hash = name_hash,
            .sketch_backend = bioseq::minimizers::
                minimizer_sketch_backend::scalar,
            .minimizer_scheme =
                bioseq::indexing::k_minimap_scheme_identity,
            .query_key = sequence_rowno,
            .query_signature = 0x7265736964656e74ULL,
            .clip_lo_bp = 0,
            .clip_hi_bp = source.locator.logical_length,
        },
    };
}

/// Check whether a relation exposes the canonical generation schema.
[[nodiscard]] inline bool relation_has_generation_schema(Oid relation_oid) {
    if (!OidIsValid(relation_oid)) return false;
    Relation relation = bioseq::postgres::support::invoke_pg_as_cpp(
        [relation_oid] {
            return bioseq::postgres::support::try_open_postgres_relation(relation_oid, AccessShareLock);
        });
    if (relation == nullptr) return false;
    const minimizer_generation_schema_inspection schema =
        bioseq::postgres::support::invoke_pg_as_cpp(
            [relation] { return inspect_minimizer_generation_schema(relation); });
    bioseq::postgres::support::invoke_pg_as_cpp(
        [relation] { relation_close(relation, AccessShareLock); });
    return static_cast<bool>(schema);
}

}  // namespace minimizer_generation_storage_test_detail

/// Integration seam for storage invariants that SQL alone cannot validate:
/// explicit clear/create rollback must rebind C++ handles, committed
/// clear/create must evict both residency forms in another backend, normal
/// ensure must fail closed on noncanonical relations, and generation clear
/// must invalidate its initiating OID handle.
[[nodiscard]] inline minimizer_generation_storage_state_fixture_result
exercise_minimizer_generation_storage_state_impl(
        Oid rel_oid,
        std::string_view index_name,
        std::uint64_t sequence_rowno,
        std::string_view operation) {
    if (!OidIsValid(rel_oid)
        || !bioseq::postgres::support::invoke_pg_as_cpp(
            [rel_oid] { return relation_oid_uses_column_table_access_method(rel_oid); })) {
        ready_minimizer_error_as_cpp(
            ERRCODE_WRONG_OBJECT_TYPE,
            "bioseq generation-storage fixture requires a USING bioseq table");
    }
    if (!bioseq::postgres::support::invoke_pg_as_cpp([] { return superuser(); })) {
        ready_minimizer_error_as_cpp(
            ERRCODE_INSUFFICIENT_PRIVILEGE,
            "bioseq generation-storage fixture requires superuser");
    }
    if (index_name != k_minimizers_per_fragment_index_name
        && index_name != k_minimizers_per_sequence_index_name) {
        ready_minimizer_error_as_cpp(
            ERRCODE_INVALID_PARAMETER_VALUE,
            "bioseq generation-storage fixture requires a minimizer index");
    }
    if (!bioseq::indexing::sql_bigint_can_represent(sequence_rowno)) {
        ready_minimizer_error_as_cpp(
            ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE,
            "bioseq generation-storage fixture query key exceeds BIGINT");
    }
    const bool savepoint_rollback = operation == "savepoint_rollback";
    const bool warm_noncanonical_residency =
        operation == "warm_noncanonical_residency";
    const bool warm_generation_residency =
        operation == "warm_generation_residency";
    const bool ensure_generation = operation == "ensure_generation";
    const bool ensure_without_clear =
        operation == "ensure_generation_without_clear";
    const bool probe_residency = operation == "probe_residency";
    const bool clear_generation = operation == "clear_generation";
    const bool clear_generation_absent =
        operation == "clear_generation_absent";
    const bool clear_generation_rollback =
        operation == "clear_generation_savepoint_rollback";
    if (!savepoint_rollback && !warm_noncanonical_residency
        && !warm_generation_residency && !ensure_generation
        && !ensure_without_clear
        && !probe_residency && !clear_generation
        && !clear_generation_absent && !clear_generation_rollback) {
        ready_minimizer_error_as_cpp(
            ERRCODE_INVALID_PARAMETER_VALUE,
            "bioseq generation-storage fixture operation is invalid",
            fmt::format("operation={}", operation));
    }

    // The next command normally drains sinval first.  Calling this explicitly
    // makes the two-backend cache validation independent of scheduler timing.
    if (probe_residency) {
        bioseq::postgres::support::invoke_pg_as_cpp(
            [] { AcceptInvalidationMessages(); });
    }

    const std::int16_t seq_attno = single_seq_attno_for_inspection(rel_oid);
    const relation_identity relation_identity = index_relation_identity(rel_oid);
    if (!OidIsValid(relation_identity.tablespace_oid)
        || !OidIsValid(relation_identity.relfilenode)) {
        ready_minimizer_error_as_cpp(
            ERRCODE_UNDEFINED_TABLE,
            "bioseq generation-storage fixture source relation is unavailable");
    }
    Snapshot active_snapshot = bioseq::postgres::support::invoke_pg_as_cpp(
        [] { return GetActiveSnapshot(); });
    if (active_snapshot == nullptr || !IsMVCCSnapshot(active_snapshot)) {
        ready_minimizer_error_as_cpp(
            ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
            "bioseq generation-storage fixture requires an active MVCC snapshot");
    }

    struct fixture_state {
        Snapshot registered_snapshot = nullptr;
        std::optional<minimizer_generation_source_authorization> source_authorization;
        minimizer_generation_source_locator source{};
        minimizer_generation_administration_identity identity{};
        std::optional<fragment_index_storage> storage;
        std::string table_name;
    };
    auto* state = new fixture_state{};
    state->identity = minimizer_generation_administration_identity{
        .database_oid = relation_identity.db_oid,
        .tablespace_oid = relation_identity.tablespace_oid,
        .main_relfilenode = relation_identity.relfilenode,
        .user_relid = rel_oid,
        .column_attno = seq_attno,
        .index_name = std::string{index_name},
    };

    minimizer_generation_storage_state_fixture_result result{};
    MemoryContext saved_context = CurrentMemoryContext;
    try {
        state->registered_snapshot = bioseq::postgres::support::invoke_pg_as_cpp(
            [active_snapshot] { return RegisterSnapshot(active_snapshot); });
        state->source_authorization.emplace(
            authorize_minimizer_generation_source_pg(
                state->identity, sequence_rowno,
                state->registered_snapshot));
        state->source = state->source_authorization->source_for_diagnostics();
        state->source_authorization.reset();
        bioseq::postgres::support::invoke_pg_as_cpp(
            [state] { UnregisterSnapshot(state->registered_snapshot); });
        state->registered_snapshot = nullptr;

        state->storage.emplace(open_fragment_storage(
            rel_oid, relation_identity.db_oid,
            relation_identity.tablespace_oid,
            relation_identity.relfilenode, seq_attno,
            state->identity.index_name, state->source.locator,
            sequence_rowno, nullptr, saved_context));
        fragment_index_storage& storage = *state->storage;
        state->table_name = storage.this_table_name();
        const Oid internal_namespace = bioseq_internal_ns();
        const auto resolve_table_oid = [state, internal_namespace] {
            return bioseq::postgres::support::invoke_pg_as_cpp(
                [state, internal_namespace] {
                return get_relname_relid(
                    state->table_name.c_str(), internal_namespace);
            });
        };
        const auto cache_keys =
            minimizer_generation_storage_test_detail::make_cache_keys(
                rel_oid, relation_identity, seq_attno,
                state->identity.index_name, state->source, sequence_rowno);

        const bool generation_input = warm_generation_residency
            || clear_generation || clear_generation_rollback;
        if (ensure_generation || ensure_without_clear) {
            result.before_oid = resolve_table_oid();
            if (!OidIsValid(result.before_oid)) {
                minimizer_generation_storage_error(
                    "generation-storage ensure fixture relation is missing");
            }
        } else if (generation_input) {
            result.before_oid = resolve_table_oid();
            if (!OidIsValid(result.before_oid)
                || !minimizer_generation_storage_test_detail::
                    relation_has_generation_schema(result.before_oid)) {
                minimizer_generation_storage_error(
                    "generation-storage fixture requires a canonical generation relation");
            }
            const Oid cached_oid = storage.resolve_relid_(false);
            if (cached_oid != result.before_oid
                || storage.index_relid_ != result.before_oid) {
                minimizer_generation_storage_error(
                    "generation-storage fixture could not cache the generation OID");
            }
        } else if (!probe_residency && !clear_generation_absent) {
            Relation noncanonical_relation =
                bioseq::postgres::support::invoke_pg_as_cpp([&storage] {
                return storage.open_index_relation_(
                    false, AccessShareLock);
            });
            if (noncanonical_relation == nullptr) {
                minimizer_generation_storage_error(
                    "generation-storage fixture noncanonical relation is missing");
            }
            result.before_oid = RelationGetRelid(noncanonical_relation);
            bioseq::postgres::support::invoke_pg_as_cpp(
                [noncanonical_relation] {
                    relation_close(noncanonical_relation, AccessShareLock);
                });
        } else {
            result.before_oid = resolve_table_oid();
        }

        if (warm_noncanonical_residency || warm_generation_residency) {
            constexpr std::array<char, 8> k_chunk_sentinel{
                'r', 'e', 's', 'i', 'd', 'e', 'n', 't'};
            const auto stored_chunk = store_cached_chunk(
                cache_keys.chunk,
                std::span<const char>{k_chunk_sentinel.data(),
                    k_chunk_sentinel.size()},
                /*written_in_current_xact=*/false);
            minimizer_hashmap_fragment_index assembled_sentinel{};
            assembled_sentinel.target_len = std::max<std::uint64_t>(
                state->source.locator.logical_length, 1u);
            store_assembled_index(
                cache_keys.assembled, std::move(assembled_sentinel));
            const bool chunk_hit =
                stored_chunk.has_value()
                && lookup_cached_chunk(cache_keys.chunk).has_value();
            const bool assembled_hit =
                lookup_assembled_index(cache_keys.assembled).has_value();
            if (!chunk_hit || !assembled_hit) {
                minimizer_generation_storage_error(
                    "generation-storage fixture could not warm both residency forms");
            }
            result.after_oid = result.before_oid;
            result.generation_schema_after = warm_generation_residency;
            result.chunk_cache_hit = chunk_hit;
            result.assembled_cache_hit = assembled_hit;
        } else if (ensure_generation) {
            if (!minimizer_generation_storage_test_detail::
                    relation_has_generation_schema(result.before_oid)) {
                // Normalize the fixture's noncanonical relation before the
                // generation ensure. The production generation clear deliberately
                // ignores noncanonical same-name relations, so use the
                // existing broad index reset seam before exercising normal
                // non-destructive generation ensure.
                storage.clear(state->identity.index_name);
                if (OidIsValid(resolve_table_oid())) {
                    minimizer_generation_storage_error(
                        "generation-storage fixture noncanonical clear failed");
                }
            }
            result.operation_oid =
                storage.ensure_minimizer_generation_storage();
            result.after_oid = resolve_table_oid();
            const bool exact =
                result.after_oid == result.operation_oid
                && minimizer_generation_storage_test_detail::
                    relation_has_generation_schema(result.after_oid);
            if (!exact) {
                minimizer_generation_storage_error(
                    "generation-storage fixture ensure did not publish the exact schema");
            }
            result.generation_schema_after = true;
        } else if (ensure_without_clear) {
            (void)storage.ensure_minimizer_generation_storage();
            minimizer_generation_storage_error(
                "generation-storage fixture unexpectedly accepted a noncanonical relation");
        } else if (probe_residency) {
            const bool chunk_hit =
                lookup_cached_chunk(cache_keys.chunk).has_value();
            const bool assembled_hit =
                lookup_assembled_index(cache_keys.assembled).has_value();
            result.after_oid = resolve_table_oid();
            result.generation_schema_after =
                minimizer_generation_storage_test_detail::
                    relation_has_generation_schema(result.after_oid);
            result.chunk_cache_hit = chunk_hit;
            result.assembled_cache_hit = assembled_hit;
        } else if (clear_generation || clear_generation_absent) {
            const minimizer_generation_clear_result cleared =
                storage.clear_minimizer_generation_storage();
            const bool expected_absent = clear_generation_absent;
            if ((expected_absent
                    && (cleared.dropped_relation_count != 0
                        || cleared.deleted_metadata_age_rows != 0))
                || (!expected_absent
                    && (cleared.dropped_relation_count == 0
                        || cleared.deleted_metadata_age_rows != 1))) {
                minimizer_generation_storage_error(fmt::format(
                    "generation clear fixture observed unexpected counts "
                    "(relations={}, metadata_age={}, expected_absent={})",
                    cleared.dropped_relation_count,
                    cleared.deleted_metadata_age_rows, expected_absent));
            }
            result.operation_oid = result.before_oid;
            result.after_oid = resolve_table_oid();
            const bool chunk_hit =
                lookup_cached_chunk(cache_keys.chunk).has_value();
            const bool assembled_hit =
                lookup_assembled_index(cache_keys.assembled).has_value();
            if (OidIsValid(result.after_oid)
                || OidIsValid(storage.index_relid_)
                || chunk_hit || assembled_hit) {
                minimizer_generation_storage_error(
                    "generation clear fixture retained physical or logical residency");
            }
            result.generation_schema_after = false;
            result.chunk_cache_hit = false;
            result.assembled_cache_hit = false;
        } else if (clear_generation_rollback) {
            result.operation_oid = result.before_oid;
            {
                auto subtransaction =
                    bioseq::postgres::support::subtransaction::begin();
                const minimizer_generation_clear_result cleared =
                    storage.clear_minimizer_generation_storage();
                const bool chunk_hit =
                    lookup_cached_chunk(cache_keys.chunk).has_value();
                const bool assembled_hit =
                    lookup_assembled_index(cache_keys.assembled).has_value();
                if (cleared.dropped_relation_count != 1
                    || cleared.deleted_metadata_age_rows != 1
                    || OidIsValid(resolve_table_oid())
                    || OidIsValid(storage.index_relid_)
                    || chunk_hit || assembled_hit) {
                    minimizer_generation_storage_error(
                        "generation clear rollback fixture did not clear inside its savepoint");
                }
                subtransaction.rollback();
            }

            bioseq::postgres::support::invoke_pg_as_cpp(
                [] { CommandCounterIncrement(); });
            result.after_oid = storage.resolve_relid_(false);
            Relation restored = OidIsValid(result.after_oid)
                ? bioseq::postgres::support::invoke_pg_as_cpp([&result] {
                    return bioseq::postgres::support::try_open_postgres_relation(
                        result.after_oid, AccessShareLock);
                })
                : nullptr;
            const bool restored_schema = restored != nullptr
                && RelationGetRelid(restored) == result.before_oid
                && minimizer_generation_storage_test_detail::
                    relation_has_generation_schema(result.after_oid);
            if (restored != nullptr) {
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [restored] {
                        relation_close(restored, AccessShareLock);
                    });
            }
            const bool chunk_hit =
                lookup_cached_chunk(cache_keys.chunk).has_value();
            const bool assembled_hit =
                lookup_assembled_index(cache_keys.assembled).has_value();
            if (!restored_schema
                || storage.index_relid_ != result.before_oid
                || chunk_hit || assembled_hit) {
                minimizer_generation_storage_error(
                    "generation clear rollback fixture did not restore and rebind safely");
            }
            result.same_handle_rebound = true;
            result.generation_schema_after = true;
            result.chunk_cache_hit = false;
            result.assembled_cache_hit = false;
        } else {
            {
                auto subtransaction =
                    bioseq::postgres::support::subtransaction::begin();
                // Savepoint rollback starts from a noncanonical relation and
                // must restore that exact object. The generation-only clear
                // cannot remove this input.
                storage.clear(state->identity.index_name);
                if (OidIsValid(resolve_table_oid())) {
                    minimizer_generation_storage_error(
                        "generation-storage rollback fixture explicit clear failed");
                }
                result.operation_oid =
                    storage.ensure_minimizer_generation_storage();
                // Deliberately seed speculative member state before rollback.
                // The relation resolver records only the authoritative name/OID;
                // it does not apply column attributes here.
                const Oid deliberately_cached = storage.resolve_relid_(false);
                if (deliberately_cached != result.operation_oid) {
                    minimizer_generation_storage_error(
                        "generation-storage fixture could not cache the speculative OID");
                }
                subtransaction.rollback();
            }

            bioseq::postgres::support::invoke_pg_as_cpp(
                [] { CommandCounterIncrement(); });
            result.after_oid = storage.resolve_relid_(false);
            Relation restored =
                bioseq::postgres::support::invoke_pg_as_cpp([&storage] {
                return storage.open_index_relation_(
                    false, AccessShareLock);
            });
            const Oid reopened_oid = restored == nullptr
                ? InvalidOid : RelationGetRelid(restored);
            if (restored != nullptr) {
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [restored] {
                        relation_close(restored, AccessShareLock);
                    });
            }
            const Oid authoritative_oid = resolve_table_oid();
            const bool rebound = OidIsValid(result.before_oid)
                && OidIsValid(result.operation_oid)
                && result.operation_oid != result.before_oid
                && result.after_oid == result.before_oid
                && reopened_oid == result.before_oid
                && authoritative_oid == result.before_oid
                && storage.index_relid_ == result.before_oid;
            if (!rebound) {
                minimizer_generation_storage_error(
                    "generation-storage fixture retained a stale speculative OID");
            }
            result.same_handle_rebound = true;
            result.generation_schema_after = false;
        }

        state->storage->close_checked();
        state->storage.reset();
        delete state;
        state = nullptr;
    } catch (...) {
        std::exception_ptr error = std::current_exception();
        if (state != nullptr) {
            state->source_authorization.reset();
            if (state->registered_snapshot != nullptr) {
                Snapshot cleanup = std::exchange(
                    state->registered_snapshot, nullptr);
                try {
                    bioseq::postgres::support::invoke_pg_as_cpp(
                        [cleanup] { UnregisterSnapshot(cleanup); });
                } catch (...) {
                    error = std::current_exception();
                }
            }
            if (state->storage.has_value()) {
                state->storage->abandon_after_pg_error_noexcept();
                state->storage.reset();
            }
            delete state;
        }
        std::rethrow_exception(error);
    }
    return result;
}

[[nodiscard]] inline std::int16_t single_seq_attno_for_inspection(Oid rel_oid) {
    const std::string sql = fmt::format(
        "SELECT a.attnum FROM pg_attribute a "
        "JOIN pg_type t ON a.atttypid=t.oid "
        "JOIN pg_namespace n ON t.typnamespace=n.oid "
        "WHERE a.attrelid={}::oid AND t.typname='seq' "
        "AND n.nspname IN ('bioseq','bioseq_internal') "
        "AND a.attnum>0 AND NOT a.attisdropped ORDER BY a.attnum",
        static_cast<unsigned>(rel_oid));
    struct sequence_attribute_number_inspection_state {
        int connect_status = 0;
        int execute_status = 0;
        int finish_status = 0;
        std::uint64_t row_count = 0;
        std::int16_t result = 0;
        bool table_present = false;
        bool is_null = true;
    } state{};
    const char* sql_data = sql.c_str();
    bioseq::postgres::support::invoke_pg_as_cpp([&state, sql_data] noexcept {
        state.connect_status = SPI_connect();
        if (state.connect_status != SPI_OK_CONNECT) return;
        state.execute_status = SPI_execute(sql_data, true, 2);
        state.row_count = SPI_processed;
        state.table_present = SPI_tuptable != nullptr;
        if (state.execute_status == SPI_OK_SELECT
            && state.table_present && state.row_count == 1) {
            bool is_null = false;
            const Datum value = SPI_getbinval(
                SPI_tuptable->vals[0], SPI_tuptable->tupdesc,
                1, &is_null);
            state.is_null = is_null;
            if (!is_null) state.result = DatumGetInt16(value);
        }
        state.finish_status = SPI_finish();
    });
    if (state.connect_status != SPI_OK_CONNECT
        || state.finish_status != SPI_OK_FINISH) {
        ready_minimizer_error_as_cpp(
            ERRCODE_INTERNAL_ERROR,
            "bioseq minimizer generation inspection could not inspect the source schema",
            fmt::format(
                "SPI_connect_status={} SPI_finish_status={}",
                state.connect_status, state.finish_status));
    }
    if (state.execute_status != SPI_OK_SELECT || !state.table_present
        || state.row_count != 1 || state.is_null || state.result <= 0) {
        ready_minimizer_error_as_cpp(
            ERRCODE_INVALID_PARAMETER_VALUE,
            "bioseq minimizer generation inspection requires exactly one bioseq.seq column",
            fmt::format("relation={} visible_seq_columns={}",
                static_cast<unsigned>(rel_oid), state.row_count));
    }
    return state.result;
}

/// Resolve and fully authenticate one READY hash-major generation. This path
/// validates the 128-bit source identity and
/// bounded-rereads every logical stream so restart audits receive recomputed
/// directory/control/posting row, byte, and CRC receipts.
[[nodiscard]] inline hash_major_minimizer_generation_inspection
inspect_hash_major_minimizer_generation(
        Oid rel_oid,
        std::string_view index_name,
        std::uint64_t sequence_rowno) {
    const bool is_bioseq_table = OidIsValid(rel_oid)
        && bioseq::postgres::support::invoke_pg_as_cpp(
            [rel_oid] { return relation_oid_uses_column_table_access_method(rel_oid); });
    if (!is_bioseq_table) {
        ready_minimizer_error_as_cpp(
            ERRCODE_WRONG_OBJECT_TYPE,
            "bioseq hash-major inspection requires a USING bioseq table");
    }
    const Oid user_id = GetUserId();
    if (!bioseq::postgres::support::invoke_pg_as_cpp(
            [rel_oid, user_id] {
                return object_ownercheck(
                    RelationRelationId, rel_oid, user_id);
            })) {
        bioseq::postgres::support::invoke_pg_as_cpp([rel_oid] {
            aclcheck_error(
                ACLCHECK_NOT_OWNER, OBJECT_TABLE, get_rel_name(rel_oid));
        });
    }
    if (index_name != k_minimizers_per_fragment_index_name
        && index_name != k_minimizers_per_sequence_index_name) {
        ready_minimizer_error_as_cpp(
            ERRCODE_INVALID_PARAMETER_VALUE,
            "bioseq hash-major inspection requires a minimizer index",
            fmt::format("index_name={}", index_name));
    }
    if (!bioseq::indexing::sql_bigint_can_represent(sequence_rowno)) {
        ready_minimizer_error_as_cpp(
            ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE,
            "bioseq hash-major inspection query key exceeds BIGINT");
    }
    if (bioseq::aligner::settings::minimizer::kmer_size != 15
        || bioseq::aligner::settings::minimizer::window_size != 10) {
        ready_minimizer_error_as_cpp(
            ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
            "bioseq hash-major inspection requires the registered k=15,w=10 profile");
    }

    const std::int16_t seq_attno =
        single_seq_attno_for_inspection(rel_oid);
    const relation_identity relation_identity =
        index_relation_identity(rel_oid);
    if (!OidIsValid(relation_identity.db_oid)
        || !OidIsValid(relation_identity.tablespace_oid)
        || !OidIsValid(relation_identity.relfilenode)) {
        ready_minimizer_error_as_cpp(
            ERRCODE_UNDEFINED_TABLE,
            "bioseq hash-major inspection source relation is unavailable");
    }
    Snapshot active_snapshot = GetActiveSnapshot();
    if (active_snapshot == nullptr || !IsMVCCSnapshot(active_snapshot)) {
        ready_minimizer_error_as_cpp(
            ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
            "bioseq hash-major inspection requires an active MVCC snapshot");
    }

    Snapshot registered_snapshot = nullptr;
    hash_major_minimizer_generation_inspection result{};
    try {
        registered_snapshot = bioseq::postgres::support::invoke_pg_as_cpp(
            [active_snapshot] { return RegisterSnapshot(active_snapshot); });
        if (registered_snapshot == nullptr
            || registered_snapshot->regd_count == 0) {
            ready_minimizer_error_as_cpp(
                ERRCODE_INTERNAL_ERROR,
                "bioseq hash-major inspection could not register its snapshot");
        }

        const minimizer_generation_administration_identity identity{
            .database_oid = relation_identity.db_oid,
            .tablespace_oid = relation_identity.tablespace_oid,
            .main_relfilenode = relation_identity.relfilenode,
            .user_relid = rel_oid,
            .column_attno = seq_attno,
            .index_name = std::string{index_name},
        };
        const ready_hash_major_minimizer_metadata_identity inspected =
            inspect_ready_hash_major_minimizer_metadata_pg(
                identity, sequence_rowno, registered_snapshot);
        const std::string table_name = format_seqidx_name(
            index_name,
            relation_identity.db_oid,
            relation_identity.tablespace_oid,
            relation_identity.relfilenode,
            seq_attno,
            inspected.source.locator.bucket_index,
            inspected.source.locator.shard_index);
        if (table_name.empty()
            || table_name.size() >= result.table_name.size()
            || index_name.size() >= result.index_name.size()) {
            ready_minimizer_error_as_cpp(
                ERRCODE_NAME_TOO_LONG,
                "bioseq hash-major inspection identity name is too long");
        }

        const auto expected_profile = bioseq::indexing::
            hash_major_minimizer_profile_for_backend(
                resolve_index_sketch_backend_from_gucs(),
                inspected.source.alphabet_code,
                15u,
                10u,
                inspected.source.locator.logical_length);
        if (!expected_profile
            || inspected.manifest.storage_profile
                != *expected_profile
            || inspected.head.query_key != sequence_rowno
            || inspected.source.sequence_rowno != sequence_rowno
            || inspected.head.source_epoch
                != inspected.source.locator.source_epoch
            || inspected.manifest.source_epoch
                != inspected.source.locator.source_epoch
            || inspected.head.source_content_crc64
                != inspected.source_content_checksum
            || inspected.manifest.source_content_crc64
                != inspected.source_content_checksum
            || inspected.manifest.target_length
                != inspected.source.locator.logical_length
            || inspected.head.source_identity
                != bioseq::indexing::hash_major_minimizer_source_identity(
                    sequence_rowno,
                    inspected.source.locator.source_epoch,
                    inspected.source_content_checksum,
                    inspected.source.locator.logical_length)
            || inspected.head.source_identity
                != inspected.manifest.source_identity
            || !inspected.source_stats.timed_equation_holds()) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "bioseq hash-major inspection source/profile identity disagrees");
        }

        static_assert(
            bioseq::indexing::k_hash_major_minimizer_head_bytes == 224u);
        static_assert(
            bioseq::indexing::k_hash_major_minimizer_manifest_bytes == 336u);
        const auto ceil_rows = [](std::uint64_t bytes) noexcept {
            return bytes
                    / bioseq::indexing::k_hash_major_minimizer_row_payload_bytes
                + (bytes
                        % bioseq::indexing::
                            k_hash_major_minimizer_row_payload_bytes
                    != 0u
                    ? 1u : 0u);
        };
        std::uint64_t expected_first_row = 0;
        for (std::size_t stream_index = 0;
             stream_index < inspected.manifest.streams.size();
             ++stream_index) {
            const auto& descriptor =
                inspected.manifest.streams[stream_index];
            const auto& catalog =
                inspected.exact_catalog_streams[stream_index];
            if (descriptor.encoding_revision != 1u
                || descriptor.row_count
                    != ceil_rows(descriptor.logical_bytes)
                || descriptor.first_row_ordinal != expected_first_row
                || catalog.kind != descriptor.kind
                || catalog.exact_catalog_rows != descriptor.row_count
                || catalog.exact_catalog_bytes
                    != descriptor.logical_bytes
                || catalog.exact_catalog_stream_crc64
                    != descriptor.stream_crc64
                || descriptor.row_count
                    > (std::numeric_limits<std::uint64_t>::max)()
                        - expected_first_row) {
                ready_minimizer_error_as_cpp(
                    ERRCODE_DATA_CORRUPTED,
                    "bioseq hash-major inspection stream receipt disagrees",
                    fmt::format("stream_index={}", stream_index));
            }
            expected_first_row += descriptor.row_count;
            result.streams[stream_index] = {
                .kind = descriptor.kind,
                .element_count = descriptor.element_count,
                .logical_bytes = descriptor.logical_bytes,
                .row_count = descriptor.row_count,
                .stream_crc64 = descriptor.stream_crc64,
                .exact_catalog_rows = catalog.exact_catalog_rows,
                .exact_catalog_bytes = catalog.exact_catalog_bytes,
                .exact_catalog_stream_crc64 =
                    catalog.exact_catalog_stream_crc64,
            };
        }
        const auto& directory = inspected.manifest.streams[0];
        const auto& controls = inspected.manifest.streams[1];
        const auto& postings = inspected.manifest.streams[2];
        const unsigned __int128 expected_directory_bytes =
            static_cast<unsigned __int128>(directory.element_count)
            * bioseq::indexing::k_hash_major_minimizer_directory_entry_bytes;
        const unsigned __int128 expected_posting_bytes =
            static_cast<unsigned __int128>(postings.element_count)
            * inspected.manifest.storage_profile
                .stored_posting_width_bytes;
        if (directory.kind
                != bioseq::indexing::hash_major_minimizer_stream_kind::directory
            || controls.kind
                != bioseq::indexing::hash_major_minimizer_stream_kind::controls
            || postings.kind
                != bioseq::indexing::hash_major_minimizer_stream_kind::postings
            || directory.element_width
                != bioseq::indexing::
                    k_hash_major_minimizer_directory_entry_bytes
            || directory.element_count
                != inspected.manifest.directory_entry_count
            || expected_directory_bytes != directory.logical_bytes
            || directory.stream_crc64
                != inspected.manifest.authenticated_directory_crc64
            || controls.element_count
                != inspected.manifest.unique_hash_count
            || controls.logical_bytes
                != inspected.manifest.total_control_bytes
            || postings.element_count
                != inspected.manifest.total_posting_count
            || postings.logical_bytes
                != inspected.manifest.total_posting_bytes
            || expected_posting_bytes != postings.logical_bytes) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "bioseq hash-major inspection manifest stream equations disagree");
        }

        result.database_oid = relation_identity.db_oid;
        result.main_relfilenode = relation_identity.relfilenode;
        result.column_attno = seq_attno;
        result.query_key = sequence_rowno;
        std::ranges::copy(table_name, result.table_name.begin());
        std::ranges::copy(index_name, result.index_name.begin());
        result.source = inspected.source;
        result.source_content_crc64 =
            inspected.source_content_checksum;
        result.source_identity = inspected.head.source_identity;
        result.head = inspected.head;
        result.manifest = inspected.manifest;
        result.encoded_head = inspected.encoded_head;
        result.encoded_manifest = inspected.encoded_manifest;
        result.head_rows = 1;
        result.head_bytes = result.encoded_head.size();
        result.manifest_rows = 1;
        result.manifest_bytes = result.encoded_manifest.size();

        Snapshot finished_snapshot =
            std::exchange(registered_snapshot, nullptr);
        bioseq::postgres::support::invoke_pg_as_cpp(
            [finished_snapshot] {
                UnregisterSnapshot(finished_snapshot);
            });
    } catch (...) {
        // The translated PostgreSQL ERROR owns snapshot cleanup.
        registered_snapshot = nullptr;
        throw;
    }
    return result;
}

/// Test-only process-parallel storage probe. Unlike the administrative
/// inspector above, this path does not serially audit all three streams before
/// the timed scan; it validates READY metadata/source authority once, then
/// delegates the exact row transport experiment to chunked_storage.
[[nodiscard]] inline hash_major_minimizer_parallel_scan_stats
hash_major_minimizer_parallel_scan_probe_impl(
        Oid rel_oid,
        std::string_view index_name,
        std::uint64_t sequence_rowno,
        std::uint32_t requested_workers) {
    const bool is_bioseq_table = OidIsValid(rel_oid)
        && bioseq::postgres::support::invoke_pg_as_cpp(
            [rel_oid] {
                return relation_oid_uses_column_table_access_method(rel_oid);
            });
    if (!is_bioseq_table) {
        ready_minimizer_error_as_cpp(
            ERRCODE_WRONG_OBJECT_TYPE,
            "bioseq hash-major parallel scan probe requires a USING bioseq table");
    }
    const Oid user_id = GetUserId();
    if (!bioseq::postgres::support::invoke_pg_as_cpp(
            [rel_oid, user_id] {
                return object_ownercheck(
                    RelationRelationId, rel_oid, user_id);
            })) {
        bioseq::postgres::support::invoke_pg_as_cpp([rel_oid] {
            aclcheck_error(
                ACLCHECK_NOT_OWNER, OBJECT_TABLE,
                get_rel_name(rel_oid));
        });
    }
    if (index_name != k_minimizers_per_fragment_index_name
        && index_name != k_minimizers_per_sequence_index_name) {
        ready_minimizer_error_as_cpp(
            ERRCODE_INVALID_PARAMETER_VALUE,
            "bioseq hash-major parallel scan probe requires a minimizer index",
            fmt::format("index_name={}", index_name));
    }
    if (!bioseq::indexing::sql_bigint_can_represent(sequence_rowno)
        || requested_workers > 64u) {
        ready_minimizer_error_as_cpp(
            ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE,
            "bioseq hash-major parallel scan probe parameter is out of range",
            fmt::format(
                "sequence_rowno={} requested_workers={}",
                sequence_rowno, requested_workers));
    }
    if (bioseq::aligner::settings::minimizer::kmer_size != 15
        || bioseq::aligner::settings::minimizer::window_size != 10) {
        ready_minimizer_error_as_cpp(
            ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
            "bioseq hash-major parallel scan probe requires the registered k=15,w=10 profile");
    }

    const std::int16_t seq_attno =
        single_seq_attno_for_inspection(rel_oid);
    const relation_identity relation_identity =
        index_relation_identity(rel_oid);
    if (!OidIsValid(relation_identity.db_oid)
        || !OidIsValid(relation_identity.tablespace_oid)
        || !OidIsValid(relation_identity.relfilenode)) {
        ready_minimizer_error_as_cpp(
            ERRCODE_UNDEFINED_TABLE,
            "bioseq hash-major parallel scan probe source relation is unavailable");
    }
    Snapshot active_snapshot = GetActiveSnapshot();
    if (active_snapshot == nullptr
        || !IsMVCCSnapshot(active_snapshot)) {
        ready_minimizer_error_as_cpp(
            ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
            "bioseq hash-major parallel scan probe requires an active MVCC snapshot");
    }

    Snapshot registered_snapshot = nullptr;
    try {
        registered_snapshot = bioseq::postgres::support::invoke_pg_as_cpp(
            [active_snapshot] {
                return RegisterSnapshot(active_snapshot);
            });
        if (registered_snapshot == nullptr
            || registered_snapshot->regd_count == 0) {
            ready_minimizer_error_as_cpp(
                ERRCODE_INTERNAL_ERROR,
                "bioseq hash-major parallel scan probe could not register its snapshot");
        }
        const minimizer_generation_administration_identity identity{
            .database_oid = relation_identity.db_oid,
            .tablespace_oid = relation_identity.tablespace_oid,
            .main_relfilenode = relation_identity.relfilenode,
            .user_relid = rel_oid,
            .column_attno = seq_attno,
            .index_name = std::string{index_name},
        };
        minimizer_generation_catalog_source_authorization authorization =
            authorize_minimizer_generation_catalog_source_pg(
                identity, sequence_rowno,
                registered_snapshot);
        hash_major_minimizer_parallel_scan_stats result =
            probe_ready_hash_major_minimizer_parallel_scan_pg(
                identity, sequence_rowno,
                registered_snapshot, std::move(authorization),
                requested_workers);
        Snapshot finished_snapshot =
            std::exchange(registered_snapshot, nullptr);
        bioseq::postgres::support::invoke_pg_as_cpp(
            [finished_snapshot] {
                UnregisterSnapshot(finished_snapshot);
            });
        return result;
    } catch (...) {
        // The SQL ERROR boundary owns registered snapshot cleanup.
        registered_snapshot = nullptr;
        throw;
    }
}

/// Resolve and fully validate one READY grouped-SoA generation under one
/// registered MVCC snapshot. This setup-only bridge reads no payload extents
/// and never enables ordinal 2. A preliminary metadata pass obtains and
/// validates the generation geometry against the active k/w and fixed ABI;
/// the existing inspector then independently revalidates the source row and
/// complete metadata directory.
[[nodiscard]] inline ready_minimizer_generation_inspection
inspect_ready_minimizer_generation_impl(
        Oid rel_oid,
        std::string_view index_name,
        std::uint64_t sequence_rowno) {
    const bool is_bioseq_table = OidIsValid(rel_oid)
        && bioseq::postgres::support::invoke_pg_as_cpp(
            [rel_oid] { return relation_oid_uses_column_table_access_method(rel_oid); });
    if (!is_bioseq_table) {
        ready_minimizer_error_as_cpp(
            ERRCODE_WRONG_OBJECT_TYPE,
            "bioseq minimizer generation inspection requires a USING bioseq table");
    }
    const Oid user_id = GetUserId();
    if (!bioseq::postgres::support::invoke_pg_as_cpp(
            [rel_oid, user_id] {
                return object_ownercheck(
                    RelationRelationId, rel_oid, user_id);
            })) {
        bioseq::postgres::support::invoke_pg_as_cpp([rel_oid] {
            aclcheck_error(
                ACLCHECK_NOT_OWNER, OBJECT_TABLE, get_rel_name(rel_oid));
        });
    }
    if (index_name != k_minimizers_per_fragment_index_name
        && index_name != k_minimizers_per_sequence_index_name) {
        ready_minimizer_error_as_cpp(
            ERRCODE_INVALID_PARAMETER_VALUE,
            "bioseq minimizer generation inspection requires a minimizer index",
            fmt::format("index_name={}", index_name));
    }
    if (!bioseq::indexing::sql_bigint_can_represent(sequence_rowno)) {
        ready_minimizer_error_as_cpp(
            ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE,
            "bioseq minimizer generation inspection query key exceeds BIGINT");
    }
    if (bioseq::aligner::settings::minimizer::kmer_size <= 0
        || bioseq::aligner::settings::minimizer::kmer_size
            > static_cast<int>((std::numeric_limits<std::uint16_t>::max)())
        || bioseq::aligner::settings::minimizer::window_size <= 0
        || bioseq::aligner::settings::minimizer::window_size
            > static_cast<int>((std::numeric_limits<std::uint16_t>::max)())) {
        ready_minimizer_error_as_cpp(
            ERRCODE_INVALID_PARAMETER_VALUE,
            "bioseq minimizer generation inspection has invalid active k/w parameters");
    }

    const std::int16_t seq_attno = single_seq_attno_for_inspection(rel_oid);
    const relation_identity relation_identity = index_relation_identity(rel_oid);
    if (!OidIsValid(relation_identity.tablespace_oid)
        || !OidIsValid(relation_identity.relfilenode)) {
        ready_minimizer_error_as_cpp(
            ERRCODE_UNDEFINED_TABLE,
            "bioseq minimizer generation inspection source relation is unavailable");
    }
    Snapshot active_snapshot = GetActiveSnapshot();
    if (active_snapshot == nullptr || !IsMVCCSnapshot(active_snapshot)) {
        ready_minimizer_error_as_cpp(
            ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
            "bioseq minimizer generation inspection requires an active MVCC snapshot");
    }

    struct inspection_state {
        Relation relation = nullptr;
        column_read_state* read_state = nullptr;
        Snapshot registered_snapshot = nullptr;
        MemoryContext work_context = nullptr;
    };
    auto* state = new inspection_state{};
    ready_minimizer_generation_inspection result{};
    const MemoryContext caller_context = CurrentMemoryContext;
    try {
        state->registered_snapshot = bioseq::postgres::support::invoke_pg_as_cpp(
            [active_snapshot] { return RegisterSnapshot(active_snapshot); });
        if (state->registered_snapshot == nullptr
            || state->registered_snapshot->regd_count == 0) {
            ready_minimizer_error_as_cpp(
                ERRCODE_INTERNAL_ERROR,
                "bioseq minimizer generation inspection could not register its snapshot");
        }
        state->work_context = bioseq::postgres::support::invoke_pg_as_cpp(
            [caller_context] {
                return AllocSetContextCreate(
                    caller_context,
                    "bioseq ready generation SQL inspection",
                    ALLOCSET_DEFAULT_SIZES);
            });
        state->relation = bioseq::postgres::support::invoke_pg_as_cpp(
            [rel_oid] {
                return bioseq::postgres::support::try_open_postgres_relation(rel_oid, AccessShareLock);
            });
        if (state->relation == nullptr
            || state->relation->rd_locator.dbOid != MyDatabaseId
            || static_cast<Oid>(state->relation->rd_locator.relNumber)
                != relation_identity.relfilenode) {
            ready_minimizer_error_as_cpp(
                ERRCODE_UNDEFINED_TABLE,
                "bioseq minimizer generation inspection source relation changed");
        }

        TupleDesc source_desc = RelationGetDescr(state->relation);
        MemoryContext previous_context =
            bioseq::postgres::support::invoke_pg_as_cpp(
                [state] {
                    return MemoryContextSwitchTo(state->work_context);
                });
        List* projected = bioseq::postgres::support::invoke_pg_as_cpp(
            [seq_attno] { return list_make1_int(seq_attno); });
        Datum* values = bioseq::postgres::support::invoke_pg_as_cpp(
            [source_desc] {
                return static_cast<Datum*>(palloc0(
                    sizeof(Datum)
                    * static_cast<std::size_t>(source_desc->natts)));
            });
        bool* nulls = bioseq::postgres::support::invoke_pg_as_cpp(
            [source_desc] {
                return static_cast<bool*>(palloc0(
                    sizeof(bool)
                    * static_cast<std::size_t>(source_desc->natts)));
            });
        state->read_state = bioseq::postgres::support::invoke_pg_as_cpp(
            [state, source_desc, projected] {
                return begin_column_read(
                    state->relation, source_desc, projected, NIL,
                    state->work_context, state->registered_snapshot,
                    /*randomAccess=*/true, nullptr);
            });
        const bool row_visible = bioseq::postgres::support::invoke_pg_as_cpp(
            [state, sequence_rowno, values, nulls] {
                return read_column_row_by_number(
                    state->read_state, sequence_rowno, values, nulls);
            });
        if (!row_visible || nulls[seq_attno - 1]) {
            bioseq::postgres::support::invoke_pg_as_cpp(
                [previous_context] {
                    MemoryContextSwitchTo(previous_context);
                });
            ready_minimizer_error_as_cpp(
                ERRCODE_NO_DATA_FOUND,
                "bioseq minimizer generation inspection source row is not visible",
                fmt::format("sequence_rowno={}", sequence_rowno));
        }
        bioseq::pg_types::seq* header = bioseq::postgres::support::invoke_pg_as_cpp(
            [values, seq_attno] {
                return reinterpret_cast<bioseq::pg_types::seq*>(
                    PG_DETOAST_DATUM(values[seq_attno - 1]));
            });
        if (!is_locator_stub(header)) {
            bioseq::postgres::support::invoke_pg_as_cpp(
                [previous_context] {
                    MemoryContextSwitchTo(previous_context);
                });
            ready_minimizer_error_as_cpp(
                ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
                "bioseq minimizer generation source has no source epoch");
        }
        const sequence_locator locator = decode_locator_from_stub(header);
        if (locator.source_epoch == 0) {
            bioseq::postgres::support::invoke_pg_as_cpp(
                [previous_context] {
                    MemoryContextSwitchTo(previous_context);
                });
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "bioseq minimizer generation source epoch is invalid");
        }
        result.source = minimizer_generation_source_locator{
            .sequence_rowno = sequence_rowno,
            .locator = locator,
            .alphabet_code = static_cast<std::uint8_t>(header->alphabet_code),
            .storage_unit_byte_width = logical_storage_unit_byte_width(header),
            .padding_letters = header->padding_letters,
            .storage_contract_checksum = 0,
        };
        bioseq::postgres::support::invoke_pg_as_cpp(
            [state] { end_column_read(state->read_state); });
        state->read_state = nullptr;
        bioseq::postgres::support::invoke_pg_as_cpp(
            [previous_context] {
                MemoryContextSwitchTo(previous_context);
            });
        Relation finished_relation = std::exchange(state->relation, nullptr);
        bioseq::postgres::support::invoke_pg_as_cpp(
            [finished_relation] {
                relation_close(finished_relation, AccessShareLock);
            });

        const std::string table_name = format_seqidx_name(
            index_name,
            relation_identity.db_oid,
            relation_identity.tablespace_oid,
            relation_identity.relfilenode,
            seq_attno,
            result.source.locator.bucket_index,
            result.source.locator.shard_index);
        const minimizer_generation_administration_identity admin_identity{
            .database_oid = relation_identity.db_oid,
            .tablespace_oid = relation_identity.tablespace_oid,
            .main_relfilenode = relation_identity.relfilenode,
            .user_relid = rel_oid,
            .column_attno = seq_attno,
            .index_name = std::string{index_name},
            .table_name = table_name,
        };

        minimizer_generation_head preliminary_head{};
        std::uint64_t expected_source_identity = 0;
        const auto discovery_expected = bioseq::indexing::
            expected_grouped_soa_generation_identity_for_backend(
            bioseq::indexing::minimizer_generation_layout::
                grouped_soa_segment_local,
            resolve_index_sketch_backend_from_gucs(),
            result.source.alphabet_code,
            result.source.storage_unit_byte_width,
            static_cast<std::uint16_t>(bioseq::aligner::settings::minimizer::kmer_size),
            static_cast<std::uint16_t>(bioseq::aligner::settings::minimizer::window_size),
            bioseq::indexing::k_grouped_soa_default_max_occurrence_cap);
        if (!discovery_expected) {
            ready_minimizer_error_as_cpp(
                ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
                "bioseq minimizer generation discovery storage contract is invalid");
        }
        {
            index_storage_fragment_generation_read_session discovery{
                table_name, sequence_rowno, state->registered_snapshot,
                *discovery_expected,
                relation_identity.tablespace_oid,
                minimizer_generation_snapshot_registration::borrow_registered};
            preliminary_head =
                discovery.discover_active_head_layout_and_close();
        }
        if (preliminary_head.layout
                != bioseq::indexing::minimizer_generation_layout::
                    grouped_soa_segment_local) {
            ready_minimizer_error_as_cpp(
                ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
                "bioseq minimizer generation wire layout is unsupported");
        }
        const auto expected = bioseq::indexing::
            expected_grouped_soa_generation_identity_for_backend(
                preliminary_head.layout,
                resolve_index_sketch_backend_from_gucs(),
                result.source.alphabet_code,
                result.source.storage_unit_byte_width,
                static_cast<std::uint16_t>(bioseq::aligner::settings::minimizer::kmer_size),
                static_cast<std::uint16_t>(bioseq::aligner::settings::minimizer::window_size),
                bioseq::indexing::k_grouped_soa_default_max_occurrence_cap);
        if (!expected) {
            ready_minimizer_error_as_cpp(
                ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
                "bioseq minimizer generation expected storage contract is invalid");
        }
        {
            index_storage_fragment_generation_read_session preliminary{
                table_name, sequence_rowno, state->registered_snapshot,
                *expected,
                relation_identity.tablespace_oid,
                minimizer_generation_snapshot_registration::borrow_registered};
            const minimizer_generation_head exact_head =
                preliminary.read_active_head_once();
            if (exact_head != preliminary_head) {
                ready_minimizer_error_as_cpp(
                    ERRCODE_DATA_CORRUPTED,
                    "bioseq minimizer generation head changed during layout binding");
            }
            const minimizer_generation_read_metadata& metadata =
                preliminary.read_manifest_directory_once(exact_head);
            result.manifest = metadata.manifest;
            result.source.storage_contract_checksum = bioseq::indexing::
                compute_minimizer_generation_storage_contract_checksum(
                    result.manifest);
            expected_source_identity = minimizer_generation_source_identity(
                relation_identity.relfilenode, result.source);
            preliminary.close_metadata_read();
        }
        if (expected_source_identity == 0) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "bioseq minimizer generation inspection source identity is invalid");
        }
        if (result.manifest.kmer_size
                != static_cast<std::uint16_t>(bioseq::aligner::settings::minimizer::kmer_size)
            || result.manifest.window_size
                != static_cast<std::uint16_t>(bioseq::aligner::settings::minimizer::window_size)
            || result.manifest.alphabet_code != result.source.alphabet_code
            || result.manifest.storage_unit_byte_width
                != result.source.storage_unit_byte_width
            || result.manifest.target_length
                != result.source.locator.logical_length
            || result.manifest.gpu_abi_revision
                != expected->gpu_abi_revision
            || result.manifest.segment_geometry_revision
                != bioseq::indexing::k_minimizer_segment_geometry_revision
            || result.source.storage_contract_checksum
                != bioseq::indexing::compute_minimizer_generation_storage_contract_checksum(
                    result.manifest)) {
            ready_minimizer_error_as_cpp(
                ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
                "bioseq minimizer generation does not match the active session storage contract",
                fmt::format("manifest k={} w={} active k={} w={}",
                    static_cast<unsigned>(result.manifest.kmer_size),
                    static_cast<unsigned>(result.manifest.window_size),
                    bioseq::aligner::settings::minimizer::kmer_size, bioseq::aligner::settings::minimizer::window_size));
        }
        result.identity = inspect_ready_minimizer_generation_identity_pg(
            admin_identity, sequence_rowno, state->registered_snapshot,
            *expected);
        if (result.identity.generation_id != preliminary_head.generation_id
            || result.identity.source_identity
                != preliminary_head.source_identity
            || result.identity.storage_contract_checksum
                != result.source.storage_contract_checksum) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "bioseq minimizer generation identity changed during inspection");
        }
        result.source_content_checksum =
            result.identity.source_content_checksum;
        result.database_oid = relation_identity.db_oid;
        result.main_relfilenode = relation_identity.relfilenode;
        result.column_attno = seq_attno;

        Snapshot finished_snapshot = std::exchange(
            state->registered_snapshot, nullptr);
        bioseq::postgres::support::invoke_pg_as_cpp(
            [finished_snapshot] { UnregisterSnapshot(finished_snapshot); });
        MemoryContext finished_context = std::exchange(
            state->work_context, nullptr);
        bioseq::postgres::support::invoke_pg_as_cpp(
            [finished_context] { MemoryContextDelete(finished_context); });
        delete state;
        state = nullptr;
    } catch (...) {
        MemoryContextSwitchTo(caller_context);
        state->read_state = nullptr;
        state->relation = nullptr;
        state->registered_snapshot = nullptr;
        state->work_context = nullptr;
        delete state;
        state = nullptr;
        throw;
    }
    return result;
}

/// Raise one fail-closed physical index audit error.
[[noreturn]] inline void index_audit_error(std::string_view message) {
    ereport(ERROR,
        (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
         errmsg("bioseq physical minimizer-index audit: %.*s",
            static_cast<int>(message.size()), message.data())));
    pg_unreachable();
}

/// Open the SPI session required by one index audit query.
inline void spi_connect_for_index_audit() {
    if (SPI_connect() != SPI_OK_CONNECT) {
        index_audit_error("SPI connection failed");
    }
}

/// Execute an audit query that must return one nonnegative bigint.
[[nodiscard]] inline std::int64_t index_audit_scalar_i64(
        const std::string& sql) {
    spi_connect_for_index_audit();
    const int status = SPI_execute(sql.c_str(), true, 0);
    if (status != SPI_OK_SELECT || SPI_processed != 1
        || SPI_tuptable == nullptr) {
        SPI_finish();
        index_audit_error("catalog query did not return exactly one row");
    }
    bool is_null = false;
    const Datum value = SPI_getbinval(
        SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &is_null);
    if (is_null) {
        SPI_finish();
        index_audit_error("catalog query returned NULL");
    }
    const std::int64_t result = DatumGetInt64(value);
    SPI_finish();
    if (result < 0) {
        index_audit_error("catalog query returned a negative count");
    }
    return result;
}

/// Derive every physical index-name prefix for a relation's sequence columns.
[[nodiscard]] inline std::vector<std::string> seqidx_prefixes_for(
        Oid rel_oid, std::string_view name) {
    const relation_identity rid = index_relation_identity(rel_oid);
    std::vector<std::string> prefixes;
    spi_connect_for_index_audit();
    const std::string sql = fmt::format(
        "SELECT a.attnum FROM pg_attribute a JOIN pg_type t ON a.atttypid=t.oid JOIN pg_namespace n ON t.typnamespace=n.oid "
        "WHERE a.attrelid={}::oid AND t.typname='seq' AND n.nspname IN ('bioseq','bioseq_internal') AND a.attnum>0 AND NOT a.attisdropped",
        static_cast<unsigned>(rel_oid));
    if (SPI_execute(sql.c_str(), true, 0) != SPI_OK_SELECT
        || SPI_tuptable == nullptr) {
        SPI_finish();
        index_audit_error("sequence-column catalog lookup failed");
    }
    for (std::uint64_t row = 0; row < SPI_processed; ++row) {
        bool is_null = false;
        const Datum value = SPI_getbinval(
            SPI_tuptable->vals[row], SPI_tuptable->tupdesc, 1, &is_null);
        if (is_null) {
            SPI_finish();
            index_audit_error("sequence-column catalog lookup returned NULL");
        }
        const std::int16_t attno = DatumGetInt16(value);
        prefixes.push_back(fmt::format(
            "seqidx_{}_{:016x}_", short_index_for(name),
            identity_hash64(rid.db_oid, rid.tablespace_oid,
                rid.relfilenode, attno, name)));
    }
    SPI_finish();
    if (prefixes.empty()) {
        index_audit_error("target has no visible bioseq.seq column");
    }
    return prefixes;
}

/// List physical index shard relations matching one escaped prefix.
[[nodiscard]] inline std::vector<std::string> shard_relnames_with_prefix(
        const std::string& prefix) {
    std::vector<std::string> out;
    spi_connect_for_index_audit();
    std::string escaped;
    for (const char character : prefix) {
        if (character == '_' || character == '%' || character == '\\') {
            escaped.push_back('\\');
        }
        escaped.push_back(character);
    }
    const std::string sql = fmt::format(
        "SELECT c.relname FROM pg_class c JOIN pg_namespace n ON n.oid=c.relnamespace "
        "WHERE n.nspname='bioseq_internal' AND c.relkind='r' AND c.relname LIKE '{}%' ESCAPE '\\'",
        escaped);
    if (SPI_execute(sql.c_str(), true, 0) != SPI_OK_SELECT
        || SPI_tuptable == nullptr) {
        SPI_finish();
        index_audit_error("physical index relation lookup failed");
    }
    for (std::uint64_t row = 0; row < SPI_processed; ++row) {
        char* name = SPI_getvalue(
            SPI_tuptable->vals[row], SPI_tuptable->tupdesc, 1);
        if (name == nullptr) {
            SPI_finish();
            index_audit_error("physical index relation lookup returned NULL");
        }
        out.emplace_back(name);
    }
    SPI_finish();
    return out;
}

/// Count rows in one quoted physical index shard with an optional predicate.
[[nodiscard]] inline std::int64_t count_in_shard(
        const std::string& relname,
        const std::string& where_clause) {
    return index_audit_scalar_i64(fmt::format(
        "SELECT count(*)::bigint FROM bioseq_internal.{} {}",
        quote_identifier(relname.c_str()), where_clause));
}

/// Check whether a shard has the complete grouped-generation key schema.
[[nodiscard]] inline bool shard_has_minimizer_generation_key(
        const std::string& relname) {
    const std::int64_t matching_attributes = index_audit_scalar_i64(fmt::format(
        "SELECT count(*)::bigint FROM pg_attribute "
        "WHERE attrelid='bioseq_internal.{}'::regclass "
        "AND attnum>0 AND NOT attisdropped AND ("
        "(attname='generation_id' AND atttypid='int8'::regtype) OR "
        "(attname='position_segment' AND atttypid='int4'::regtype) OR "
        "(attname='sub_key' AND atttypid='int8'::regtype) OR "
        "(attname='chunk_no' AND atttypid='int4'::regtype))",
        quote_identifier(relname.c_str())));
    return matching_attributes == 4;
}

/// Count persisted rows across every shard of one logical index.
[[nodiscard]] inline std::int64_t index_table_row_count_impl(Oid rel_oid, std::string_view name) {
    std::int64_t total = 0;
    for (const std::string& pfx : seqidx_prefixes_for(rel_oid, name))
        for (const std::string& sh : shard_relnames_with_prefix(pfx)) {
            const std::int64_t rows = count_in_shard(sh, "");
            if (rows > (std::numeric_limits<std::int64_t>::max)() - total) {
                index_audit_error("persisted row count overflow");
            }
            total += rows;
        }
    return total;
}

/// Metadata-only evidence that one minimizer index has no physical payload.
struct cold_no_index_absence_audit {
    std::uint64_t target_rows = 0;
    bool target_cardinality_metadata_only = true;
    std::uint64_t source_parent_rows_read = 0;
    std::uint64_t source_fragment_rows_read = 0;
    std::uint64_t source_decoded_bp = 0;
    std::uint64_t physical_index_relations = 0;
    std::uint64_t persisted_index_rows = 0;
    std::uint64_t generation_objects = 0;
};

/// Fail-closed, metadata-only validation for one no-index policy. It reads
/// Bioseq stripe metadata for target cardinality and PostgreSQL catalogs/index
/// relations for persisted-index absence. It never forms a target tuple and
/// therefore cannot hydrate or decode a sequence source.
[[nodiscard]] inline cold_no_index_absence_audit
cold_no_index_absence_audit_impl(Oid rel_oid, std::string_view name) {
    if (name != "minimizers_per_fragment"
        && name != "minimizers_per_sequence") {
        index_audit_error(
            "audit requires a minimizer fragment or sequence policy");
    }
    Relation target = table_open(rel_oid, AccessShareLock);
    if (!relation_oid_uses_column_table_access_method(rel_oid)) {
        table_close(target, AccessShareLock);
        index_audit_error("target is not a USING bioseq relation");
    }
    cold_no_index_absence_audit audit{
        .target_rows = column_table_row_count(target),
    };
    table_close(target, AccessShareLock);

    for (const std::string& prefix : seqidx_prefixes_for(rel_oid, name)) {
        for (const std::string& shard : shard_relnames_with_prefix(prefix)) {
            ++audit.physical_index_relations;
            const std::int64_t rows = count_in_shard(shard, "");
            if (static_cast<std::uint64_t>(rows)
                    > (std::numeric_limits<std::uint64_t>::max)()
                        - audit.persisted_index_rows) {
                index_audit_error("persisted row count overflow");
            }
            audit.persisted_index_rows += static_cast<std::uint64_t>(rows);
            if (shard_has_minimizer_generation_key(shard)) {
                const std::int64_t generations = count_in_shard(
                    shard,
                    fmt::format(
                        "WHERE generation_id=0 AND position_segment=-1 "
                        "AND sub_key={} AND chunk_no=0",
                        static_cast<long long>(k_generation_head_sub_key)));
                if (static_cast<std::uint64_t>(generations)
                        > (std::numeric_limits<std::uint64_t>::max)()
                            - audit.generation_objects) {
                    index_audit_error("generation object count overflow");
                }
                audit.generation_objects +=
                    static_cast<std::uint64_t>(generations);
            }
        }
    }
    return audit;
}

/// Return the largest per-shard distinct published sub-key count.
[[nodiscard]] inline std::int64_t index_distinct_sub_keys_impl(Oid rel_oid, std::string_view name, std::int64_t query_key) {
    std::int64_t best = 0;
    const std::string where = fmt::format("WHERE query_key={} AND sub_key < {}", (long long)query_key, (long long)k_temporary_run_sub_key_base);
    for (const std::string& pfx : seqidx_prefixes_for(rel_oid, name))
        for (const std::string& sh : shard_relnames_with_prefix(pfx)) {
            // distinct sub_key in this shard for this query_key
            if (SPI_connect() != SPI_OK_CONNECT) continue;
            std::string sql = fmt::format("SELECT count(DISTINCT sub_key)::bigint FROM bioseq_internal.{} {}", quote_identifier(sh.c_str()), where);
            if (SPI_execute(sql.c_str(), true, 0) == SPI_OK_SELECT && SPI_processed > 0) {
                bool isn=false; std::int64_t v = DatumGetInt64(SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isn));
                if (!isn && v > best) best = v;
            }
            SPI_finish();
        }
    return best;
}

/// Drop every physical index relation through the shared cleanup boundary.
inline void drop_all_index_tables_impl() {
    ErrorData* postgres_error = nullptr;
    std::array<char, 2048> cpp_error_detail{};
    bool cpp_error = false;
    bool unknown_cpp_error = false;
    try {
        drop_index_relations(
            drop_index_relation_selector::all_index_tables());
    } catch (bioseq::postgres::support::postgres_error& error) {
        postgres_error = error.release_error_data();
    } catch (const std::exception& error) {
        strlcpy(
            cpp_error_detail.data(),
            error.what() == nullptr
                ? "std::exception without detail"
                : error.what(),
            cpp_error_detail.size());
        cpp_error = true;
    } catch (...) {
        unknown_cpp_error = true;
    }

    // The SPI session and every owning C++ value have unwound before the
    // PostgreSQL C ABI is re-entered.
    if (postgres_error != nullptr)
        ReThrowError(postgres_error);
    if (cpp_error) {
        ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("bioseq index relation cleanup failed"),
             errdetail_internal("%s", cpp_error_detail.data())));
    }
    if (unknown_cpp_error) {
        ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("bioseq index relation cleanup failed"),
             errdetail_internal("unknown C++ exception")));
    }
}

/// 0 ok, 1 unknown name, 2 ineligible.
[[nodiscard]] inline int set_index_policy_impl(Oid rel_oid, std::string_view name, bool update_on_insert) {
    if (!name_is_registered(name)) return 1;
    if (update_on_insert) {
        auto flags = pg_supported_indexes::allows_on_insert_flags();
        auto types = pg_supported_indexes::list_types();
        for (std::size_t i = 0; i < types.size(); ++i) if (types[i].first == name && !flags[i]) return 2;
    }
    const relation_identity rid = index_relation_identity(rel_oid);
    if (SPI_connect() != SPI_OK_CONNECT) return 0;
    std::string sql = fmt::format(
        "INSERT INTO bioseq_internal.index_policy (user_relid, main_relfilenode, column_attno, index_name, enabled, update_on_insert) "
        "SELECT {}::oid, {}::oid, a.attnum, {}, true, {} FROM pg_attribute a JOIN pg_type t ON a.atttypid=t.oid "
        "JOIN pg_namespace n ON t.typnamespace=n.oid WHERE a.attrelid={}::oid AND t.typname='seq' AND n.nspname IN ('bioseq','bioseq_internal') "
        "AND a.attnum>0 AND NOT a.attisdropped "
        "ON CONFLICT (user_relid, main_relfilenode, column_attno, index_name) DO UPDATE SET update_on_insert=EXCLUDED.update_on_insert, enabled=true",
        static_cast<unsigned>(rel_oid),
        static_cast<unsigned>(rid.relfilenode),
        quote_literal_cstr(std::string(name).c_str()),
        update_on_insert ? "true" : "false", static_cast<unsigned>(rel_oid));
    SPI_execute(sql.c_str(), false, 0);
    SPI_finish();
    return 0;
}

/// Persist one active read index for every sequence column of a relation.
[[nodiscard]] inline int set_active_read_index_impl(Oid rel_oid, std::string_view name) {
    if (!name_is_registered(name)) return 1;
    if (layout_of_name(name) == sequence_index_layout::table_chunks) return 2;
    const relation_identity rid = index_relation_identity(rel_oid);
    if (SPI_connect() != SPI_OK_CONNECT) return 0;
    std::string clear = fmt::format(
        "UPDATE bioseq_internal.index_policy SET is_active_read=false WHERE user_relid={}::oid AND main_relfilenode={}::oid",
        static_cast<unsigned>(rel_oid),
        static_cast<unsigned>(rid.relfilenode));
    SPI_execute(clear.c_str(), false, 0);
    std::string set_ = fmt::format(
        "INSERT INTO bioseq_internal.index_policy (user_relid, main_relfilenode, column_attno, index_name, enabled, is_active_read) "
        "SELECT {}::oid, {}::oid, a.attnum, {}, true, true FROM pg_attribute a JOIN pg_type t ON a.atttypid=t.oid "
        "JOIN pg_namespace n ON t.typnamespace=n.oid WHERE a.attrelid={}::oid AND t.typname='seq' AND n.nspname IN ('bioseq','bioseq_internal') "
        "AND a.attnum>0 AND NOT a.attisdropped "
        "ON CONFLICT (user_relid, main_relfilenode, column_attno, index_name) DO UPDATE SET is_active_read=true, enabled=true",
        static_cast<unsigned>(rel_oid),
        static_cast<unsigned>(rid.relfilenode),
        quote_literal_cstr(std::string(name).c_str()),
        static_cast<unsigned>(rel_oid));
    SPI_execute(set_.c_str(), false, 0);
    SPI_finish();
    return 0;
}

}  // namespace bioseq::storage::indexing
