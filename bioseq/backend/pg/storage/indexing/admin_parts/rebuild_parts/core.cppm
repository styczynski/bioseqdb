/**
 * @file core.cppm
 * @brief Index rebuild routes and PostgreSQL error translation.
 */
module;
#include "access/heapam.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "commands/tablespace.h"
#include "executor/tuptable.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"

#include <bioseq/pg_compat.h>
#include <libassert/assert.hpp>
extern "C" {
#include "storage/buffile.h"
}
#include <algorithm>
#include <array>
#include <concepts>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <expected>
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
export module bioseq.postgres.storage.indexing.admin:rebuild_core;
import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.postgres.support;
import bioseq.postgres.sql_boundary;
import bioseq.postgres.configuration.aligner;
import bioseq.postgres.configuration.core;
import bioseq.postgres.storage.manager; // seq_caches, sequence_locator, is_locator_stub, decode_locator_from_stub
import bioseq.storage.layout;
import bioseq.storage.size;
import bioseq.storage.lazy_sequence;           // make_resident_lazy_sequence
import bioseq.storage.scanner;                 // sequence access requirements
import bioseq.postgres.storage.scanner;        // with_postgres_lazy_sequence
import bioseq.postgres.types.compact_sequence; // bioseq::pg_types::seq, dispatch_instance
import bioseq.sequences;                       // compact_sequence_range
import bioseq.common.seqan_adapter;            // seqan3::char_is_valid_for
import bioseq.indexing.type;
import bioseq.indexing.storage;
import bioseq.indexing.traits;
import bioseq.indexing.registry;
import bioseq.indexing.minimizer_hashmap;
import bioseq.indexing.hash_major_minimizer_index;
import bioseq.indexing.minimizer_sequence;
import bioseq.pipeline.runtime_contract; // minimizer_algorithm
import bioseq.sequences.kmers;           // minimizer/index APIs
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
import bioseq.postgres.storage.metadata; // column_table_row_count
static_assert(__cplusplus >= 202302L, "C++23-or-later");
import :policy_catalog;
import :hash_major;
import :grouped_soa;

export namespace bioseq::storage::indexing {

// ---- admin update_index ---------------------------------------------------
/// Check whether an index name is present in the compiled registry.
[[nodiscard]] inline bool name_is_registered(std::string_view name) {
    for (auto& t : pg_supported_indexes::list_types())
        if (t.first == name)
            return true;
    return false;
}
/// Return the physical layout registered for one index name.
[[nodiscard]] inline sequence_index_layout layout_of_name(std::string_view name) {
    return pg_supported_indexes::layout_of(name);
}

/// Run one storage build and preserve its checked success or abort cleanup.
template <class p_storage, class p_build_function>
inline void run_pg_index_build_with_cleanup(p_storage& storage, p_build_function&& build_fn) {
    auto release_runs = [&storage]() noexcept {
        if constexpr (requires(p_storage& cleanup_storage) {
                          {
                              cleanup_storage.release_runs_noexcept()
                          } noexcept -> std::same_as<void>;
                      }) {
            storage.release_runs_noexcept();
        } else if constexpr (requires(p_storage& cleanup_storage) {
                                 {
                                     cleanup_storage.release_resident_runs_noexcept()
                                 } noexcept -> std::same_as<void>;
                             }) {
            storage.release_resident_runs_noexcept();
        }
    };
    try {
        build_fn();
        if constexpr (requires(p_storage& completed_storage) {
                          { completed_storage.close_checked() } -> std::same_as<void>;
                      }) {
            storage.close_checked();
        }
    } catch (const bioseq::postgres::support::postgres_error&) {
        if constexpr (requires(p_storage& failed_storage) {
                          {
                              failed_storage.abandon_after_pg_error_noexcept()
                          } noexcept -> std::same_as<void>;
                      }) {
            storage.abandon_after_pg_error_noexcept();
        }
        throw;
    } catch (...) {
        if constexpr (requires(p_storage& failed_storage) {
                          {
                              failed_storage.abandon_after_pg_error_noexcept()
                          } noexcept -> std::same_as<void>;
                      }) {
            storage.abandon_after_pg_error_noexcept();
        } else {
            release_runs();
        }
        throw;
    }
}

/// Own an admitted resident target for a parallel sequence-index build.
template <sequence_alphabet p_alphabet> struct resident_index_build_target {
    bioseq::storage::admitted_packed_sequence<p_alphabet> owner;
};

/// Admit a fragmented target for parallel resident processing when allowed.
template <sequence_alphabet p_alphabet>
[[nodiscard]] std::optional<resident_index_build_target<p_alphabet>>
try_materialize_for_parallel_sequence_index_build(
    bioseq::storage::lazy_sequence_view<p_alphabet> seq,
    std::uint8_t requested_width,
    std::uint64_t rowno,
    std::string_view index_name) {
    bioseq::postgres::scheduler::backend_sync();
    auto& pool = bioseq::parallelism::global_pool::instance();
    if (!bioseq::parallelism::has_parallel_tasks(pool.get_mode()) ||
        pool.maximum_concurrency() <= 1) {
        return std::nullopt;
    }
    const auto access = bioseq::storage::plan_sequence_access(
        seq,
        bioseq::storage::sequence_access_requirement::contiguous_resident,
        0u,
        seq.bp_size(),
        requested_width);
    if (access.route == bioseq::storage::sequence_access_route::borrow_resident) {
        return std::nullopt;
    }
    constexpr std::uint64_t k_context_reserve_bytes = std::uint64_t{8} << 20;
    const std::uint64_t reserve_bytes =
        access.admission_bytes >
                (std::numeric_limits<std::uint64_t>::max)() - k_context_reserve_bytes
            ? (std::numeric_limits<std::uint64_t>::max)()
            : access.admission_bytes + k_context_reserve_bytes;
    auto permit =
        bioseq::parallelism::memory_admission_controller::instance().try_reserve(reserve_bytes);
    if (!permit) {
        bioseq_log<logging_level::debug>(
            "[bioseq] index: row={} index={} target_bp={} admitted_bytes={} "
            "not admitted for resident parallel build; using rank stream",
            static_cast<unsigned long long>(rowno),
            index_name,
            static_cast<unsigned long long>(seq.bp_size()),
            static_cast<unsigned long long>(access.admission_bytes));
        return std::nullopt;
    }
#ifdef BIOSEQ_TEST_INDEXES
    note_pg_sequence_facade_materialization_begin();
#endif
    auto owner = bioseq::storage::materialize_packed_sequence(
        seq, 0u, seq.bp_size(), requested_width, std::move(*permit));
#ifdef BIOSEQ_TEST_INDEXES
    note_pg_sequence_facade_materialization_complete();
#endif
    bioseq_log<logging_level::debug>(
        "[bioseq] index: row={} index={} target_bp={} admitted_bytes={} "
        "materialized for resident parallel build",
        static_cast<unsigned long long>(rowno),
        index_name,
        static_cast<unsigned long long>(seq.bp_size()),
        static_cast<unsigned long long>(access.admission_bytes));
    return resident_index_build_target<p_alphabet>{std::move(owner)};
}

// Whole-target resident materialization is disabled under Vulkan: the goal is
// streaming fragmented GPU windows, not making the fragmented target
// contiguous on the host.
/// Rebuild one fragment index through a resident or streamed source route.
template <class p_supported_indexes, class p_storage, sequence_alphabet p_alphabet>
inline void
rebuild_one_fragmented_maybe_resident(p_supported_indexes& idx,
                                      std::string_view name,
                                      p_storage& store,
                                      bioseq::storage::lazy_sequence_view<p_alphabet> sequence,
                                      std::uint8_t requested_width,
                                      std::uint8_t alphabet_code,
                                      std::uint64_t rowno,
                                      bioseq::minimizers::minimizer_sketch_backend backend,
                                      const bioseq::minimizers::minimizer_kernels* kernels) {
    if (backend == bioseq::minimizers::minimizer_sketch_backend::scalar &&
        name == bioseq::indexing::k_minimizers_per_sequence_index_name) {
        if (auto resident = try_materialize_for_parallel_sequence_index_build(
                sequence, requested_width, rowno, name);
            resident.has_value()) {
            idx.rebuild_one_fragmented(
                name, store, resident->owner.view(), backend, kernels, alphabet_code);
            return;
        }
    }
    idx.rebuild_one_fragmented(name, store, sequence, backend, kernels, alphabet_code);
}

/**
 * Adapt one source-domain-promoted Datum to the canonical lazy facade for the
 * duration of an index-build callback.  The read session owns every facade
 * adapter and is closed before the caller resets the row context containing
 * the expanded Datum.
 */
template <sequence_alphabet p_alphabet, class p_callback>
inline void with_promoted_pg_sequence_view(Datum promoted_datum,
                                           MemoryContext parent_context,
                                           p_callback&& callback) {
    const pg_sequence_descriptor descriptor = bioseq::postgres::support::invoke_pg_as_cpp(
        [promoted_datum] { return classify_and_describe_sequence(promoted_datum); });
    std::exception_ptr callback_failure;
    pg_sequence_read_session* volatile session = nullptr;
    PG_TRY();
    {
        auto* local_session = begin_pg_sequence_read_session(parent_context);
        session = local_session;
        try {
            auto* owner =
                make_pg_sequence_call_owner<p_alphabet>(*local_session, promoted_datum, descriptor);
            std::forward<p_callback>(callback)(owner->view());
            finish_pg_sequence_read_session(local_session, true);
        } catch (...) {
            callback_failure = std::current_exception();
            abort_pg_sequence_read_session_noexcept(local_session);
        }
        session = nullptr;
    }
    PG_CATCH();
    {
        abort_pg_sequence_read_session_noexcept(const_cast<pg_sequence_read_session*>(session));
        session = nullptr;
        PG_RE_THROW();
    }
    PG_END_TRY();
    if (callback_failure != nullptr) {
        std::rethrow_exception(std::move(callback_failure));
    }
}

/// Opt-in locator-backed minimizer hash-major publication bridge. The existing
/// persistence GUC selects this production branch without introducing another
/// feature flag; grouped-SoA generations remain readable.
template <class p_sequence_range>
inline void build_and_verify_hash_major_minimizer_generation_pg(
    Oid user_relid,
    std::int16_t column_attno,
    std::string_view index_name,
    std::uint64_t sequence_rowno,
    fragment_index_storage& storage,
    bioseq::storage::lazy_sequence_view<
        typename bioseq::storage::postgres_lazy_sequence_binding<p_sequence_range>::alphabet_type>
        sequence,
    bioseq::storage::postgres_lazy_sequence_binding<p_sequence_range> source_binding,
    bioseq::minimizers::minimizer_sketch_backend backend,
    const bioseq::minimizers::minimizer_kernels* kernels);

/// Publish one locator-backed grouped-SoA minimizer generation.
template <class p_sequence_range>
inline void build_and_verify_grouped_soa_minimizer_generation_pg(
    Oid user_relid,
    std::int16_t column_attno,
    std::string_view index_name,
    std::uint64_t sequence_rowno,
    fragment_index_storage& storage,
    bioseq::storage::lazy_sequence_view<
        typename bioseq::storage::postgres_lazy_sequence_binding<p_sequence_range>::alphabet_type>
        sequence,
    bioseq::storage::postgres_lazy_sequence_binding<p_sequence_range> source_binding,
    bioseq::minimizers::minimizer_sketch_backend backend,
    const bioseq::minimizers::minimizer_kernels* kernels);

/// Reject a grouped-SoA rebuild before mixing inline and locator sources.
[[noreturn]] inline void reject_grouped_soa_nonlocator_source_as_cpp(std::string_view index_name) {
    bioseq::postgres::support::invoke_pg_as_cpp([index_name] {
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("grouped-SoA persistence requires exclusively "
                        "PostgreSQL locator-backed sequence sources"),
                 errdetail("index %.*s encountered an inline source and would "
                           "otherwise mix grouped-SoA and scheme-untagged payload rows under "
                           "one physical name",
                           static_cast<int>(index_name.size()),
                           index_name.data()),
                 errhint("Disable grouped-SoA persistence for inline-source rebuilds or "
                         "rewrite every source through the bioseq table AM.")));
    });
    pg_unreachable();
}

/// Fail before the destructive index clear if the grouped-SoA rebuild
/// cannot use one homogeneous PostgreSQL-locator source domain. The table AM
/// normally guarantees locator stubs; this scan makes that prerequisite an
/// enforced storage contract rather than an assumption about current writes.
inline void
verify_grouped_soa_rebuild_sources_are_locators(Oid rel_oid,
                                                std::span<const std::int16_t> seq_attnos,
                                                std::string_view index_name,
                                                MemoryContext parent_context) {
    struct preflight_state {
        Relation relation = nullptr;
        TupleDesc tuple_desc = nullptr;
        Snapshot registered_snapshot = nullptr;
        MemoryContext scan_context = nullptr;
        MemoryContext row_context = nullptr;
        column_read_state* read_state = nullptr;
    } state{};
    std::exception_ptr failure;
    try {
        state.relation = bioseq::postgres::support::invoke_pg_as_cpp([rel_oid] {
            return bioseq::postgres::support::try_open_postgres_relation(rel_oid, AccessShareLock);
        });
        if (state.relation == nullptr) {
            bioseq::postgres::support::invoke_pg_as_cpp([rel_oid] {
                ereport(ERROR,
                        (errcode(ERRCODE_UNDEFINED_TABLE),
                         errmsg("grouped-SoA source preflight relation is unavailable"),
                         errdetail("relation_oid=%u", static_cast<unsigned>(rel_oid))));
            });
            pg_unreachable();
        }
        state.tuple_desc = bioseq::postgres::support::invoke_pg_as_cpp(
            [&state] { return RelationGetDescr(state.relation); });
        state.registered_snapshot = bioseq::postgres::support::invoke_pg_as_cpp(
            [] { return RegisterSnapshot(GetTransactionSnapshot()); });
        state.scan_context = bioseq::postgres::support::invoke_pg_as_cpp([parent_context] {
            return AllocSetContextCreate(
                parent_context, "bioseq_grouped_soa_source_preflight", ALLOCSET_DEFAULT_SIZES);
        });
        state.row_context = bioseq::postgres::support::invoke_pg_as_cpp([&state] {
            return AllocSetContextCreate(state.scan_context,
                                         "bioseq_grouped_soa_source_preflight_row",
                                         ALLOCSET_DEFAULT_SIZES);
        });
        List* projected = bioseq::postgres::support::invoke_pg_as_cpp([&state, seq_attnos] {
            const MemoryContext previous = MemoryContextSwitchTo(state.scan_context);
            List* result = nullptr;
            for (const std::int16_t attno : seq_attnos) {
                result = lappend_int(result, static_cast<int>(attno));
            }
            MemoryContextSwitchTo(previous);
            return result;
        });
        state.read_state = bioseq::postgres::support::invoke_pg_as_cpp([&state, projected] {
            return begin_column_read(state.relation,
                                     state.tuple_desc,
                                     projected,
                                     NIL,
                                     state.scan_context,
                                     state.registered_snapshot,
                                     /*randomAccess=*/false,
                                     nullptr);
        });
        std::vector<Datum> values(static_cast<std::size_t>(state.tuple_desc->natts));
        std::vector<char> null_buffer(static_cast<std::size_t>(state.tuple_desc->natts));
        bool* nulls = reinterpret_cast<bool*>(null_buffer.data());
        std::uint64_t rowno = 0;
        while (bioseq::postgres::support::invoke_pg_as_cpp([&state, &values, nulls, &rowno] {
            return read_next_column_row(state.read_state, values.data(), nulls, &rowno);
        })) {
            bioseq::postgres::support::invoke_pg_as_cpp(
                [&state] { MemoryContextReset(state.row_context); });
            for (const std::int16_t attno : seq_attnos) {
                if (nulls[attno - 1]) {
                    continue;
                }
                bioseq::pg_types::seq* header =
                    bioseq::postgres::support::invoke_pg_as_cpp([&state, &values, attno] {
                        const MemoryContext previous = MemoryContextSwitchTo(state.row_context);
                        bioseq::pg_types::seq* result = reinterpret_cast<bioseq::pg_types::seq*>(
                            PG_DETOAST_DATUM(values[attno - 1]));
                        MemoryContextSwitchTo(previous);
                        return result;
                    });
                if (!is_locator_stub(header)) {
                    reject_grouped_soa_nonlocator_source_as_cpp(index_name);
                }
            }
        }
    } catch (...) {
        failure = std::current_exception();
    }

    // One exit owns every leaf cleanup. Cleanup failures are retained only
    // when there is no earlier source/preflight failure to preserve.
    const auto cleanup_leaf = [&failure](auto&& function) {
        try {
            function();
        } catch (...) {
            if (failure == nullptr) {
                failure = std::current_exception();
            }
        }
    };
    if (state.read_state != nullptr) {
        column_read_state* read_state = std::exchange(state.read_state, nullptr);
        cleanup_leaf([read_state] {
            bioseq::postgres::support::invoke_pg_as_cpp(
                [read_state] { end_column_read(read_state); });
        });
    }
    if (state.registered_snapshot != nullptr) {
        Snapshot snapshot = std::exchange(state.registered_snapshot, nullptr);
        cleanup_leaf([snapshot] {
            bioseq::postgres::support::invoke_pg_as_cpp(
                [snapshot] { UnregisterSnapshot(snapshot); });
        });
    }
    if (state.row_context != nullptr) {
        MemoryContext context = std::exchange(state.row_context, nullptr);
        cleanup_leaf([context] {
            bioseq::postgres::support::invoke_pg_as_cpp(
                [context] { MemoryContextDelete(context); });
        });
    }
    if (state.relation != nullptr) {
        Relation relation = std::exchange(state.relation, nullptr);
        cleanup_leaf([relation] {
            bioseq::postgres::support::invoke_pg_as_cpp(
                [relation] { relation_close(relation, AccessShareLock); });
        });
    }
    if (state.scan_context != nullptr) {
        MemoryContext context = std::exchange(state.scan_context, nullptr);
        cleanup_leaf([context] {
            bioseq::postgres::support::invoke_pg_as_cpp(
                [context] { MemoryContextDelete(context); });
        });
    }
    if (failure != nullptr) {
        std::rethrow_exception(failure);
    }
}

/// Rebuild one registered index across every sequence column and visible row.
inline std::int64_t update_index_impl(Oid rel_oid, std::string_view name) {
    if (!name_is_registered(name)) {
        bioseq::postgres::support::invoke_pg_as_cpp([name] {
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("bioseq.update_index received an unknown index"),
                     errdetail("index_name=%.*s", static_cast<int>(name.size()), name.data())));
        });
        pg_unreachable();
    }
    const bool is_bioseq_table = bioseq::postgres::support::invoke_pg_as_cpp(
        [rel_oid] { return relation_oid_uses_column_table_access_method(rel_oid); });
    if (!is_bioseq_table) {
        bioseq::postgres::support::invoke_pg_as_cpp([] {
            ereport(ERROR,
                    (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                     errmsg("bioseq.update_index relation is not a USING bioseq table")));
        });
        pg_unreachable();
    }
    const relation_identity rid = index_relation_identity(rel_oid);
    const sequence_index_layout layout = layout_of_name(name);
    const auto backend = resolve_index_sketch_backend_from_gucs();
    const bool is_minimizer_index =
        name == bioseq::indexing::k_minimizers_per_fragment_index_name ||
        name == bioseq::indexing::k_minimizers_per_sequence_index_name;
    const bioseq::minimizers::minimizer_kernels* const minimizer_kernels =
        is_minimizer_index
            ? selected_minimizer_kernels_for_operation(backend, "bioseq.update_index")
            : nullptr;
    const bool use_grouped_soa_generation =
        is_minimizer_index && bioseq::aligner::settings::grouped_soa_generation::enabled;
    if (is_minimizer_index) {
        // Cold fail-closed gate before generation-id allocation, relation/file
        // creation, payload writes, or descriptor/head publication.
        bioseq::storage::validate_minimizer_generation_storage_catalogs_pg();
    }
    reset_last_minimizer_sequence_build_stats();
    if (use_grouped_soa_generation &&
        (bioseq::settings::index::async_persistence::enabled ||
         bioseq::settings::index::recompute_async_persistence::enabled)) {
        bioseq::postgres::support::invoke_pg_as_cpp([] {
            ereport(
                ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("grouped-SoA persistence requires asynchronous "
                        "persistence to be disabled"),
                 errdetail("bioseq.index.async_persistence.enabled=%s, "
                           "bioseq.index.recompute_async_persistence.enabled=%s",
                           bioseq::settings::index::async_persistence::enabled ? "on" : "off",
                           bioseq::settings::index::recompute_async_persistence::enabled ? "on"
                                                                                         : "off"),
                 errhint("Set bioseq.index.async_persistence.enabled=off and "
                         "bioseq.index.recompute_async_persistence.enabled=off before the "
                         "controlled grouped-SoA rebuild.")));
        });
        pg_unreachable();
    }
    ensure_vulkan_index_build_runtime_ready(backend);
    bioseq_log<logging_level::debug>(
        "[bioseq] index: update_index name={} backend={} vulkan_enabled={} "
        "vulkan_gpu_index_build={} minimizer_algorithm={} grouped-SoA={}",
        name,
        bioseq::minimizers::minimizer_sketch_backend_name(backend),
        bioseq::aligner::settings::vulkan::enabled,
        bioseq::aligner::settings::vulkan::index_build::enabled,
        static_cast<int>(bioseq::aligner::settings::minimizer::algorithm.get_value()),
        use_grouped_soa_generation);

    // Locate every bioseq.seq column: the index covers each sequence column;
    // mmsearch(target, query) queries the target column, which may not be the first).
    std::vector<std::int16_t> seq_attnos;
    const std::string seq_attno_sql =
        fmt::format("SELECT a.attnum FROM pg_attribute a JOIN pg_type t "
                    "ON a.atttypid=t.oid JOIN pg_namespace n ON t.typnamespace=n.oid "
                    "WHERE a.attrelid={}::oid AND t.typname='seq' "
                    "AND n.nspname IN ('bioseq','bioseq_internal') "
                    "AND a.attnum>0 AND NOT a.attisdropped ORDER BY a.attnum",
                    static_cast<unsigned>(rel_oid));
    struct sequence_attribute_number_spi_state {
        int connect_status = 0;
        int execute_status = 0;
        int finish_status = 0;
        std::uint64_t row_count = 0;
        std::uint64_t null_row = (std::numeric_limits<std::uint64_t>::max)();
        bool table_present = false;
        bool overflow = false;
        std::array<std::int16_t, MaxTupleAttributeNumber> values{};
    } state{};
    const char* seq_attno_sql_data = seq_attno_sql.c_str();
    bioseq::postgres::support::invoke_pg_as_cpp([&state, seq_attno_sql_data] noexcept {
        state.connect_status = SPI_connect();
        if (state.connect_status != SPI_OK_CONNECT)
            return;
        state.execute_status = SPI_execute(seq_attno_sql_data, true, 0);
        state.row_count = SPI_processed;
        state.table_present = SPI_tuptable != nullptr;
        state.overflow = state.row_count > state.values.size();
        if (state.execute_status == SPI_OK_SELECT && state.table_present && !state.overflow) {
            for (std::uint64_t row = 0; row < state.row_count; ++row) {
                bool is_null = false;
                const Datum value =
                    SPI_getbinval(SPI_tuptable->vals[row], SPI_tuptable->tupdesc, 1, &is_null);
                if (is_null) {
                    state.null_row = row;
                    break;
                }
                state.values[static_cast<std::size_t>(row)] = DatumGetInt16(value);
            }
        }
        state.finish_status = SPI_finish();
    });
    if (state.connect_status != SPI_OK_CONNECT || state.execute_status != SPI_OK_SELECT ||
        !state.table_present || state.overflow ||
        state.null_row != (std::numeric_limits<std::uint64_t>::max)() ||
        state.finish_status != SPI_OK_FINISH) {
        bioseq::postgres::support::invoke_pg_as_cpp([&state] {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("bioseq.update_index sequence-column query failed"),
                     errdetail("SPI_connect_status=%d SPI_execute_status=%d "
                               "rows=%llu tuptable=%s overflow=%s null_row=%llu "
                               "SPI_finish_status=%d",
                               state.connect_status,
                               state.execute_status,
                               static_cast<unsigned long long>(state.row_count),
                               state.table_present ? "present" : "null",
                               state.overflow ? "true" : "false",
                               static_cast<unsigned long long>(state.null_row),
                               state.finish_status)));
        });
        pg_unreachable();
    }
    seq_attnos.assign(state.values.begin(),
                      state.values.begin() + static_cast<std::ptrdiff_t>(state.row_count));
    if (seq_attnos.empty()) {
        bioseq::postgres::support::invoke_pg_as_cpp([] {
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("bioseq.update_index relation has no bioseq.seq column")));
        });
        pg_unreachable();
    }

    const MemoryContext parent_context = CurrentMemoryContext;
    MemoryContext work = bioseq::postgres::support::invoke_pg_as_cpp([parent_context] {
        return AllocSetContextCreate(parent_context, "bioseq_update_index", ALLOCSET_DEFAULT_SIZES);
    });
    pg_supported_indexes idx;
    idx.apply_table_defaults();

    if (use_grouped_soa_generation) {
        try {
            verify_grouped_soa_rebuild_sources_are_locators(rel_oid, seq_attnos, name, work);
        } catch (...) {
            std::exception_ptr failure = std::current_exception();
            try {
                bioseq::postgres::support::invoke_pg_as_cpp([work] { MemoryContextDelete(work); });
            } catch (...) {
                // Preserve the source-contract failure; transaction abort owns
                // any context whose defensive delete also failed.
            }
            std::rethrow_exception(failure);
        }
    }

    // Replace-in-place layouts still require a destructive
    // clear.  Generation-backed minimizers instead append a new immutable
    // positive generation to the existing canonical sidecar and atomically
    // replace only generation zero's head.  Clearing here would drop the
    // relation, invalidate its OID/relfilenode, and erase the rollback/GC
    // predecessor before publication.
    if (!use_grouped_soa_generation) {
        for (std::int16_t a : seq_attnos) {
            acquire_index_advisory_xact_lock(
                rid.db_oid, rid.tablespace_oid, rid.relfilenode, a, name);
            auto f = open_fragment_storage(rel_oid,
                                           rid.db_oid,
                                           rid.tablespace_oid,
                                           rid.relfilenode,
                                           a,
                                           name,
                                           sequence_locator{},
                                           0,
                                           nullptr,
                                           work);
            auto q = open_sequence_storage(rel_oid,
                                           rid.db_oid,
                                           rid.tablespace_oid,
                                           rid.relfilenode,
                                           a,
                                           name,
                                           sequence_locator{},
                                           0,
                                           nullptr,
                                           work);
            auto tb = open_table_storage(
                rel_oid, rid.db_oid, rid.tablespace_oid, rid.relfilenode, a, name, nullptr, work);
            try {
                idx.clear_index(f, q, tb, name);
                f.close_checked();
                q.close_checked();
                tb.close_checked();
            } catch (...) {
                f.abandon_after_pg_error_noexcept();
                q.abandon_after_pg_error_noexcept();
                tb.abandon_after_pg_error_noexcept();
                throw;
            }
        }
    }

    seq_caches* caches = bioseq::postgres::support::invoke_pg_as_cpp(
        [work] { return bioseq::storage::alloc_seq_caches(work); });
    caches->side = bioseq::storage::seq_caches::access_side::read;
    caches->lock_mode = AccessShareLock;
    caches->read_state_mctx = work;
    caches->main_database_oid = rid.db_oid;
    caches->main_tablespace_oid = rid.tablespace_oid;
    caches->main_relfilenode = rid.relfilenode;

    if (layout == sequence_index_layout::table_chunks) {
        std::int64_t written = 0;
        if (Relation ur =
                bioseq::postgres::support::try_open_postgres_relation(rel_oid, AccessShareLock);
            ur != nullptr) {
            written = static_cast<std::int64_t>(column_table_row_count(ur));
            relation_close(ur, AccessShareLock);
        }
        for (std::int16_t a : seq_attnos) {
            auto tb = open_table_storage(
                rel_oid, rid.db_oid, rid.tablespace_oid, rid.relfilenode, a, name, caches, work);
            run_pg_index_build_with_cleanup(
                tb, [&idx, &name, &tb] { idx.rebuild_one_table(name, tb); });
        }
        for (std::int16_t a : seq_attnos)
            note_successful_write(rel_oid, rid.relfilenode, a, name);
        caches->close_all(/*commit=*/false); // close cached seq_sub read-state relations (else
                                             // "resource was not closed" at commit)
        bioseq::storage::destroy_seq_caches(caches);
        MemoryContextDelete(work);
        return written;
    }

    // per-sequence walk: build the index for every SEQ column of every row.
    Relation ur = nullptr;
    TupleDesc utd = nullptr;
    Snapshot snap = nullptr;
    column_read_state* rs = nullptr;
    MemoryContext inner = nullptr;
    bioseq::storage::pg_sequence_source_provider* source_provider = nullptr;
    // Read each row's stored value via the column reader, not
    // table_scan_getnextslot. The bioseq table-AM scan assembles the full
    // sequence into one varlena, which exceeds MaxAllocSize (~1 GiB) for a multi-Gbp
    // target (6 Gbp => ~1.5 GB) and ERRORs "sequence too large to materialise as a
    // single varlena". read_next_column_row returns the RAW stored value (the 56-byte
    // locator stub for a large sequence); we decode the locator and STREAM fragments
    // via scanner_pg (RSS bounded by one fragment) — the only path that can index a
    // >MaxAllocSize target. `&rowno` is the columnar row number == query_key (no
    // tid_to_row_number needed; that path also fixed the rowno source).
    std::int64_t written = 0;
    std::exception_ptr row_walk_failure;
    try {
        ur = bioseq::postgres::support::invoke_pg_as_cpp([rel_oid] {
            return bioseq::postgres::support::try_open_postgres_relation(rel_oid, AccessShareLock);
        });
        if (ur == nullptr) {
            bioseq::postgres::support::invoke_pg_as_cpp([rel_oid] {
                ereport(ERROR,
                        (errcode(ERRCODE_UNDEFINED_TABLE),
                         errmsg("bioseq.update_index source relation is unavailable"),
                         errdetail("relation_oid=%u", static_cast<unsigned>(rel_oid))));
            });
            pg_unreachable();
        }
        utd = bioseq::postgres::support::invoke_pg_as_cpp([ur] { return RelationGetDescr(ur); });
        snap = bioseq::postgres::support::invoke_pg_as_cpp(
            [] { return RegisterSnapshot(GetTransactionSnapshot()); });
        List* projected = bioseq::postgres::support::invoke_pg_as_cpp([work, &seq_attnos] {
            const MemoryContext previous = MemoryContextSwitchTo(work);
            List* result = nullptr;
            for (const std::int16_t attno : seq_attnos) {
                result = lappend_int(result, static_cast<int>(attno));
            }
            MemoryContextSwitchTo(previous);
            return result;
        });
        rs = bioseq::postgres::support::invoke_pg_as_cpp([ur, utd, projected, work, snap] {
            return begin_column_read(ur,
                                     utd,
                                     projected,
                                     NIL,
                                     work,
                                     snap,
                                     /*randomAccess=*/false,
                                     nullptr);
        });
        // BeginRead may flush pending writes and replace its snapshot with a
        // newly registered one. The provider and retained hash-major source
        // authorization must observe that same authoritative snapshot.
        source_provider =
            bioseq::storage::create_standalone_pg_sequence_source_provider(ur, rs->snapshot);
        inner = bioseq::postgres::support::invoke_pg_as_cpp([work] {
            return AllocSetContextCreate(
                work, "bioseq_update_index_inner_seq", ALLOCSET_DEFAULT_SIZES);
        });
        std::vector<Datum> vals(static_cast<std::size_t>(utd->natts));
        std::vector<char> nullbuf(static_cast<std::size_t>(utd->natts));
        Datum* valp = vals.data();
        bool* nulls = reinterpret_cast<bool*>(nullbuf.data());
        std::uint64_t rowno = 0;
        while (bioseq::postgres::support::invoke_pg_as_cpp(
            [rs, valp, nulls, &rowno] { return read_next_column_row(rs, valp, nulls, &rowno); })) {
            bioseq::postgres::support::invoke_pg_as_cpp([inner] { MemoryContextReset(inner); });
            // Bound RSS to one row's decode plus one source fragment.
            bool any_col = false;
            for (std::int16_t seq_attno : seq_attnos) {
                if (nulls[seq_attno - 1])
                    continue;
                bioseq::pg_types::seq* hdr =
                    bioseq::postgres::support::invoke_pg_as_cpp([inner, valp, seq_attno] {
                        const MemoryContext previous = MemoryContextSwitchTo(inner);
                        bioseq::pg_types::seq* result = reinterpret_cast<bioseq::pg_types::seq*>(
                            PG_DETOAST_DATUM(valp[seq_attno - 1]));
                        MemoryContextSwitchTo(previous);
                        return result;
                    });
                const std::uint8_t alpha = hdr->alphabet_code;
                const std::uint8_t bw = logical_storage_unit_byte_width(hdr);
                if (is_minimizer_index && !use_grouped_soa_generation &&
                    backend == bioseq::minimizers::minimizer_sketch_backend::bounded_nthash_simd) {
                    reject_bounded_nthash_for_scheme_untagged_fragment_layout(
                        backend, "bioseq.update_index");
                }
                if (is_locator_stub(hdr)) {
                    const sequence_locator loc = decode_locator_from_stub(hdr);
                    const Datum promoted_datum = bioseq::storage::promote_pg_sequence_locator(
                        source_provider, seq_attno, PointerGetDatum(hdr), inner);
                    bioseq::pg_types::seq::dispatch_instance(
                        alpha,
                        bw,
                        [rid,
                         seq_attno,
                         loc,
                         rs,
                         caches,
                         inner,
                         promoted_datum,
                         layout,
                         rel_oid,
                         name,
                         rowno,
                         work,
                         &idx,
                         use_grouped_soa_generation,
                         backend,
                         minimizer_kernels,
                         bw,
                         alpha]<class p_sequence_type>() {
                            using source_binding =
                                bioseq::storage::postgres_lazy_sequence_binding<p_sequence_type>;
                            const source_binding bound_source{
                                rid.db_oid,
                                rid.tablespace_oid,
                                rid.relfilenode,
                                seq_attno,
                                loc,
                                source_binding::expected_last_fragment_padding(loc),
                                rs->snapshot,
                                caches,
                                inner};
                            bioseq::postgres::support::invoke_pg_as_cpp([promoted_datum,
                                                                         inner,
                                                                         layout,
                                                                         rel_oid,
                                                                         rid,
                                                                         seq_attno,
                                                                         &name,
                                                                         loc,
                                                                         rowno,
                                                                         caches,
                                                                         work,
                                                                         &idx,
                                                                         use_grouped_soa_generation,
                                                                         bound_source,
                                                                         backend,
                                                                         minimizer_kernels,
                                                                         bw,
                                                                         alpha] {
                                with_promoted_pg_sequence_view<
                                    typename source_binding::alphabet_type>(
                                    promoted_datum,
                                    inner,
                                    [layout,
                                     rel_oid,
                                     rid,
                                     seq_attno,
                                     &name,
                                     loc,
                                     rowno,
                                     caches,
                                     work,
                                     &idx,
                                     use_grouped_soa_generation,
                                     bound_source,
                                     backend,
                                     minimizer_kernels,
                                     bw,
                                     alpha](auto lazy_view) {
                                        if (layout == sequence_index_layout::sequence_inline) {
                                            auto store = open_sequence_storage(rel_oid,
                                                                               rid.db_oid,
                                                                               rid.tablespace_oid,
                                                                               rid.relfilenode,
                                                                               seq_attno,
                                                                               name,
                                                                               loc,
                                                                               rowno,
                                                                               caches,
                                                                               work);
                                            run_pg_index_build_with_cleanup(
                                                store, [&idx, &name, &store, lazy_view] {
                                                    idx.rebuild_one_inline(name, store, lazy_view);
                                                });
                                        } else if (use_grouped_soa_generation) {
                                            auto store = open_fragment_storage(rel_oid,
                                                                               rid.db_oid,
                                                                               rid.tablespace_oid,
                                                                               rid.relfilenode,
                                                                               seq_attno,
                                                                               name,
                                                                               loc,
                                                                               rowno,
                                                                               caches,
                                                                               work);
                                            build_and_verify_hash_major_minimizer_generation_pg(
                                                rel_oid,
                                                seq_attno,
                                                name,
                                                rowno,
                                                store,
                                                lazy_view,
                                                bound_source,
                                                backend,
                                                minimizer_kernels);
                                        } else {
                                            auto store = open_fragment_storage(rel_oid,
                                                                               rid.db_oid,
                                                                               rid.tablespace_oid,
                                                                               rid.relfilenode,
                                                                               seq_attno,
                                                                               name,
                                                                               loc,
                                                                               rowno,
                                                                               caches,
                                                                               work);
                                            run_pg_index_build_with_cleanup(
                                                store,
                                                [&idx,
                                                 &name,
                                                 &store,
                                                 lazy_view,
                                                 bw,
                                                 alpha,
                                                 rowno,
                                                 backend,
                                                 minimizer_kernels] {
                                                    rebuild_one_fragmented_maybe_resident(
                                                        idx,
                                                        name,
                                                        store,
                                                        lazy_view,
                                                        bw,
                                                        alpha,
                                                        rowno,
                                                        backend,
                                                        minimizer_kernels);
                                                });
                                        }
                                    });
                            });
                        });
                } else {
                    if (use_grouped_soa_generation) {
                        reject_grouped_soa_nonlocator_source_as_cpp(name);
                    }
                    bioseq::pg_types::seq::dispatch_instance(
                        alpha,
                        hdr->storage_unit_byte_width,
                        [hdr,
                         layout,
                         rel_oid,
                         rid,
                         seq_attno,
                         name,
                         rowno,
                         caches,
                         work,
                         &idx,
                         bw,
                         backend,
                         minimizer_kernels,
                         alpha]<class p_sequence_type>() {
                            compact_sequence_range<p_sequence_type> rng{*hdr};
                            auto lazy = bioseq::storage::make_resident_lazy_sequence(rng);
                            const auto lazy_view = bioseq::storage::make_lazy_sequence_view(lazy);
                            sequence_locator loc{};
                            loc.logical_length = rng.bp_size();
                            loc.bucket_index = static_cast<std::uint8_t>(
                                bioseq::storage::
                                    sequence_bucket_index<bioseq::storage::sequence_size_t, 8, 42>(
                                        loc.logical_length));
                            loc.shard_index = 0;
                            if (layout == sequence_index_layout::sequence_inline) {
                                auto store = open_sequence_storage(rel_oid,
                                                                   rid.db_oid,
                                                                   rid.tablespace_oid,
                                                                   rid.relfilenode,
                                                                   seq_attno,
                                                                   name,
                                                                   loc,
                                                                   rowno,
                                                                   caches,
                                                                   work);
                                run_pg_index_build_with_cleanup(
                                    store, [&idx, &name, &store, lazy_view] {
                                        idx.rebuild_one_inline(name, store, lazy_view);
                                    });
                            } else {
                                auto store = open_fragment_storage(rel_oid,
                                                                   rid.db_oid,
                                                                   rid.tablespace_oid,
                                                                   rid.relfilenode,
                                                                   seq_attno,
                                                                   name,
                                                                   loc,
                                                                   rowno,
                                                                   caches,
                                                                   work);
                                run_pg_index_build_with_cleanup(
                                    store,
                                    [&idx,
                                     &name,
                                     &store,
                                     lazy_view,
                                     bw,
                                     alpha,
                                     rowno,
                                     backend,
                                     minimizer_kernels] {
                                        rebuild_one_fragmented_maybe_resident(idx,
                                                                              name,
                                                                              store,
                                                                              lazy_view,
                                                                              bw,
                                                                              alpha,
                                                                              rowno,
                                                                              backend,
                                                                              minimizer_kernels);
                                    });
                            }
                        });
                }
                any_col = true; // detoast copies live in `inner`, reclaimed by the per-row reset /
                                // final delete
            }
            if (any_col)
                ++written;
        }
        // Release the last expanded source handle before the provider performs
        // its checked commit sweep.  Earlier rows were released by their next
        // per-row reset.
        bioseq::postgres::support::invoke_pg_as_cpp([inner] { MemoryContextReset(inner); });
        bioseq::postgres::support::invoke_pg_as_cpp([source_provider] {
            bioseq::storage::close_pg_sequence_source_provider(source_provider, /*is_commit=*/true);
        });
        source_provider = nullptr;
    } catch (...) {
        row_walk_failure = std::current_exception();
    }

    // One cleanup exit preserves the biological/build error while attempting
    // every PostgreSQL leaf. Fragment caches borrow rs->snapshot, so they close
    // before end_column_read can unregister a refreshed snapshot copy.
    const auto cleanup_leaf = [&row_walk_failure](auto&& function) {
        try {
            function();
        } catch (...) {
            if (row_walk_failure == nullptr) {
                row_walk_failure = std::current_exception();
            }
        }
    };
    // Expanded Datums retain their source domains.  The normal path reset the
    // row context before its checked sweep; do the same before an abort sweep
    // so no retained domain survives until transaction end.
    if (inner != nullptr) {
        MemoryContext context = std::exchange(inner, nullptr);
        cleanup_leaf([context] {
            bioseq::postgres::support::invoke_pg_as_cpp(
                [context] { MemoryContextDelete(context); });
        });
    }
    if (source_provider != nullptr) {
        auto* provider = std::exchange(source_provider, nullptr);
        cleanup_leaf([provider] {
            bioseq::storage::close_pg_sequence_source_provider(provider, /*is_commit=*/false);
        });
    }
    if (caches != nullptr) {
        cleanup_leaf([caches] {
            bioseq::postgres::support::invoke_pg_as_cpp(
                [caches] { caches->close_all(/*commit=*/false); });
        });
    }
    if (rs != nullptr) {
        column_read_state* read_state = std::exchange(rs, nullptr);
        cleanup_leaf([read_state] {
            bioseq::postgres::support::invoke_pg_as_cpp(
                [read_state] { end_column_read(read_state); });
        });
    }
    if (snap != nullptr) {
        Snapshot snapshot = std::exchange(snap, nullptr);
        cleanup_leaf([snapshot] {
            bioseq::postgres::support::invoke_pg_as_cpp(
                [snapshot] { UnregisterSnapshot(snapshot); });
        });
    }
    if (ur != nullptr) {
        Relation relation = std::exchange(ur, nullptr);
        cleanup_leaf([relation] {
            bioseq::postgres::support::invoke_pg_as_cpp(
                [relation] { relation_close(relation, AccessShareLock); });
        });
    }
    if (caches != nullptr) {
        seq_caches* finished_caches = std::exchange(caches, nullptr);
        cleanup_leaf([finished_caches] {
            bioseq::postgres::support::invoke_pg_as_cpp(
                [finished_caches] { bioseq::storage::destroy_seq_caches(finished_caches); });
        });
    }
    if (work != nullptr) {
        MemoryContext context = std::exchange(work, nullptr);
        cleanup_leaf([context] {
            bioseq::postgres::support::invoke_pg_as_cpp(
                [context] { MemoryContextDelete(context); });
        });
    }
    if (row_walk_failure != nullptr) {
        std::rethrow_exception(row_walk_failure);
    }
    for (std::int16_t a : seq_attnos) {
        note_successful_write(rel_oid, rid.relfilenode, a, name);
    }
    return written;
}

/// Map an Exact SIMD runtime rejection to the stable PostgreSQL error contract.
[[noreturn]] inline void raise_exact_simd_index_pg_error(const char* operation,
                                                         const char* detail) {
    ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("Exact SIMD minimizer index build is unsupported for this source"),
             errdetail_internal("%s: %s", operation, detail),
             errhint("Exact SIMD index builds have no scalar fallback; use DNA4 with "
                     "a supported packed storage width and SIMD target, or select a "
                     "different minimizer algorithm explicitly.")));
    pg_unreachable();
}

/// Map a bounded-ntHash status to its stable PostgreSQL SQLSTATE class.
[[nodiscard]] constexpr int packed_bounded_nthash_index_pg_errcode(
    bioseq::indexing::packed_bounded_nthash_index_status status) noexcept {
    using status_type = bioseq::indexing::packed_bounded_nthash_index_status;
    switch (status) {
    case status_type::unsupported_alphabet:
    case status_type::unsupported_storage:
    case status_type::unsupported_representation:
    case status_type::unsupported_vector_shape:
        return ERRCODE_FEATURE_NOT_SUPPORTED;
    case status_type::input_too_large:
    case status_type::admission_denied:
        return ERRCODE_PROGRAM_LIMIT_EXCEEDED;
    case status_type::invalid_state:
        return ERRCODE_INTERNAL_ERROR;
    case status_type::invalid_configuration:
    case status_type::input_too_short:
    case status_type::invalid_range:
        return ERRCODE_INVALID_PARAMETER_VALUE;
    case status_type::ok:
        return ERRCODE_INTERNAL_ERROR;
    }
    return ERRCODE_INTERNAL_ERROR;
}

static_assert(packed_bounded_nthash_index_pg_errcode(
                  bioseq::indexing::packed_bounded_nthash_index_status::unsupported_alphabet) ==
              ERRCODE_FEATURE_NOT_SUPPORTED);
static_assert(packed_bounded_nthash_index_pg_errcode(
                  bioseq::indexing::packed_bounded_nthash_index_status::unsupported_vector_shape) ==
              ERRCODE_FEATURE_NOT_SUPPORTED);
static_assert(packed_bounded_nthash_index_pg_errcode(
                  bioseq::indexing::packed_bounded_nthash_index_status::admission_denied) ==
              ERRCODE_PROGRAM_LIMIT_EXCEEDED);
static_assert(packed_bounded_nthash_index_pg_errcode(
                  bioseq::indexing::packed_bounded_nthash_index_status::input_too_large) ==
              ERRCODE_PROGRAM_LIMIT_EXCEEDED);
static_assert(packed_bounded_nthash_index_pg_errcode(
                  bioseq::indexing::packed_bounded_nthash_index_status::invalid_state) ==
              ERRCODE_INTERNAL_ERROR);
static_assert(packed_bounded_nthash_index_pg_errcode(
                  bioseq::indexing::packed_bounded_nthash_index_status::ok) ==
              ERRCODE_INTERNAL_ERROR);

/// Raise the PostgreSQL error corresponding to one bounded-ntHash status.
[[noreturn]] inline void raise_packed_bounded_nthash_index_pg_error(
    bioseq::indexing::packed_bounded_nthash_index_status status,
    const char* operation,
    const char* detail) {
    const int sqlerrcode = packed_bounded_nthash_index_pg_errcode(status);
    const std::string_view status_name =
        bioseq::indexing::packed_bounded_nthash_index_status_name(status);
    const char* message =
        sqlerrcode == ERRCODE_FEATURE_NOT_SUPPORTED
            ? "Bounded ntHash SIMD minimizer index build is unsupported for this source"
        : sqlerrcode == ERRCODE_PROGRAM_LIMIT_EXCEEDED
            ? "Bounded ntHash SIMD minimizer index build exceeded a resource limit"
        : sqlerrcode == ERRCODE_INTERNAL_ERROR
            ? "Bounded ntHash SIMD minimizer index status contract was violated"
            : "Bounded ntHash SIMD minimizer index build rejected its request";
    ereport(ERROR,
            (errcode(sqlerrcode),
             errmsg("%s", message),
             errdetail_internal("%s: status=%.*s %s",
                                operation,
                                static_cast<int>(status_name.size()),
                                status_name.data(),
                                detail),
             errhint("Bounded ntHash SIMD has no scalar or Exact SIMD fallback; "
                     "use DNA4 with packed uint32/uint64 storage and W=10/K=15, "
                     "or increase the index memory limit when "
                     "status=admission_denied.")));
    pg_unreachable();
}

/// SQL-facing boundary: no C++ execution error may cross the PostgreSQL C ABI.
[[nodiscard]] inline std::int64_t
update_index_pg_checked(Oid rel_oid, const char* name_data, std::size_t name_size) {
    ErrorData* exact_error = nullptr;
    std::array<char, 2048> detail{};
    auto packed_bounded_status =
        bioseq::indexing::packed_bounded_nthash_index_status::invalid_state;
    bool exact_simd_error = false;
    bool packed_bounded_error = false;
    bool cpp_error = false;
    bool unknown_cpp_error = false;
    try {
        const std::string name{name_data, name_size};
        return update_index_impl(rel_oid, name);
    } catch (bioseq::postgres::support::postgres_error& error) {
        exact_error = error.release_error_data();
    } catch (const bioseq::indexing::exact_simd_index_runtime_error& err) {
        strlcpy(detail.data(),
                err.what() == nullptr ? "Exact SIMD error without detail" : err.what(),
                detail.size());
        exact_simd_error = true;
    } catch (const bioseq::indexing::packed_bounded_nthash_index_runtime_error& err) {
        strlcpy(detail.data(),
                err.what() == nullptr ? "Bounded ntHash SIMD error without detail" : err.what(),
                detail.size());
        packed_bounded_status = err.status();
        packed_bounded_error = true;
    } catch (const std::exception& error) {
        strlcpy(detail.data(),
                error.what() == nullptr ? "std::exception without detail" : error.what(),
                detail.size());
        cpp_error = true;
    } catch (...) {
        unknown_cpp_error = true;
    }
    // Every owning index-build object has unwound before either PostgreSQL
    // terminal path below.  Only fixed/trivial diagnostic state remains.
    if (exact_error != nullptr) {
        ReThrowError(exact_error);
    }
    if (exact_simd_error) {
        raise_exact_simd_index_pg_error("bioseq.update_index", detail.data());
    }
    if (packed_bounded_error) {
        raise_packed_bounded_nthash_index_pg_error(
            packed_bounded_status, "bioseq.update_index", detail.data());
    }
    if (cpp_error) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("bioseq.update_index raised a C++ exception"),
                 errdetail_internal("%s", detail.data())));
    }
    if (unknown_cpp_error) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("bioseq.update_index raised an unknown C++ exception")));
    }
    pg_unreachable();
}

// Rebuild every policy-enabled UPDATE_ON_INSERT index for
// `main_relid` in ONE batched pass (one streaming rebuild per index, no per-row
// advisory locks). bioseq.load calls this once after its streamed insert when
// bioseq.index.insert_maintenance.enabled is on. It loads the same per-table
// policy as
// `index_build_on_insert`, honors every eligible UPDATE_ON_INSERT index, and
// drives `update_index_impl`, which rebuilds across
// every seq column and every row — so the observable index state matches a
// normal INSERT (only faster: one tuple-lock-free streaming pass).
/// Production caller-side seal: publication must preserve the requested
/// backend and exact source/generation identities returned by the encoder.
template <class p_sequence_range>
inline void build_and_verify_hash_major_minimizer_generation_pg(
    Oid user_relid,
    std::int16_t column_attno,
    std::string_view index_name,
    std::uint64_t sequence_rowno,
    fragment_index_storage& storage,
    bioseq::storage::lazy_sequence_view<
        typename bioseq::storage::postgres_lazy_sequence_binding<p_sequence_range>::alphabet_type>
        sequence,
    bioseq::storage::postgres_lazy_sequence_binding<p_sequence_range> source_binding,
    bioseq::minimizers::minimizer_sketch_backend backend,
    const bioseq::minimizers::minimizer_kernels* kernels) {
    using source_binding_type = bioseq::storage::postgres_lazy_sequence_binding<p_sequence_range>;
    const sequence_locator& locator = source_binding.locator();
    constexpr std::uint16_t k_window_size = 10u;
    constexpr std::uint16_t k_kmer_size = 15u;
    constexpr std::uint16_t k_alphabet_code = source_binding_type::alphabet_code();
    if (index_name == k_minimizers_per_fragment_index_name) {
        // The fragment route owns one persisted index per physical source fragment. Never
        // route it through the target-wide hash-major spill/merge publisher.
        build_and_verify_grouped_soa_minimizer_generation_pg(user_relid,
                                                             column_attno,
                                                             index_name,
                                                             sequence_rowno,
                                                             storage,
                                                             sequence,
                                                             source_binding,
                                                             backend,
                                                             kernels);
        return;
    }
    if constexpr (k_alphabet_code != 0u) {
        // Avoid instantiating the large hash-major publisher for alphabet
        // specializations which are statically outside its registry.
        build_and_verify_grouped_soa_minimizer_generation_pg(user_relid,
                                                             column_attno,
                                                             index_name,
                                                             sequence_rowno,
                                                             storage,
                                                             sequence,
                                                             source_binding,
                                                             backend,
                                                             kernels);
        return;
    } else {
        const auto profile = bioseq::indexing::hash_major_minimizer_profile_for_backend(
            backend, k_alphabet_code, k_kmer_size, k_window_size, locator.logical_length);
        if (!profile) {
            // Future DNA4 parameter profiles which are not in the hash-major
            // registry continue on the grouped-SoA route.
            build_and_verify_grouped_soa_minimizer_generation_pg(user_relid,
                                                                 column_attno,
                                                                 index_name,
                                                                 sequence_rowno,
                                                                 storage,
                                                                 sequence,
                                                                 source_binding,
                                                                 backend,
                                                                 kernels);
            return;
        }
        const ready_hash_major_minimizer_publication publication =
            build_and_publish_hash_major_minimizer_generation_pg(user_relid,
                                                                 column_attno,
                                                                 index_name,
                                                                 sequence_rowno,
                                                                 storage,
                                                                 sequence,
                                                                 source_binding,
                                                                 backend,
                                                                 kernels);
        if (!publication.published || publication.generation_id == 0u ||
            publication.query_key != sequence_rowno ||
            publication.source_epoch != locator.source_epoch ||
            publication.selected_backend != backend ||
            publication.head.generation_id != publication.generation_id ||
            publication.head.query_key != sequence_rowno ||
            publication.head.source_epoch != locator.source_epoch ||
            publication.manifest.generation_id != publication.generation_id ||
            publication.manifest.source_epoch != locator.source_epoch ||
            publication.manifest.target_length != locator.logical_length ||
            publication.unique_hash_count != publication.manifest.unique_hash_count ||
            publication.total_posting_count != publication.manifest.total_posting_count) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "normal hash-major minimizer rebuild publication validation was rejected",
                fmt::format("index={} backend={}",
                            index_name,
                            bioseq::minimizers::minimizer_sketch_backend_name(backend)));
        }
    }
}

} // namespace bioseq::storage::indexing
