/**
 * @file admin/rebuild/core.cppm
 * @brief Rebuild indexes from stored sequences and report failures to PostgreSQL.
 */
module;

extern "C" {
#include "postgres.h"

#include "access/htup_details.h"
#include "access/relation.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "nodes/pg_list.h"
#include "port.h"
#include "utils/errcodes.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
}

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <exception>
#include <fmt/format.h>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module bioseq.postgres.storage.indexing.admin:rebuild_core;
import bioseq.common.logging;
import bioseq.pipeline.runtime_contract;
import bioseq.postgres.support;
import bioseq.postgres.configuration.aligner;
import bioseq.postgres.configuration.core;
import bioseq.postgres.storage.manager; // seq_caches, sequence_locator, is_locator_stub, decode_locator_from_stub
import bioseq.storage.layout;
import bioseq.storage.size;
import bioseq.storage.lazy_sequence;           // make_resident_lazy_sequence
import bioseq.storage.scanner;                 // sequence access requirements
import bioseq.postgres.storage.scanner;        // PostgreSQL sequence read sessions
import bioseq.postgres.types.compact_sequence; // bioseq::pg_types::seq, dispatch_instance
import bioseq.sequences;                       // compact_sequence_range
import bioseq.indexing.type;
import bioseq.indexing.symbol_counts;
import bioseq.sequences.cigar;
import bioseq.indexing.minimizer_hashmap;
import bioseq.sequences.kmers.highway_minimizer_contract;
import bioseq.postgres.storage.indexing.chunked_storage;
import bioseq.parallelism.memory_admission;
import bioseq.parallelism.mode;
import bioseq.parallelism.pool;
import bioseq.postgres.scheduler.supervisor;
import bioseq.postgres.table.io;
import bioseq.postgres.storage.indexing.fragment_handle;
import bioseq.postgres.storage.indexing.index_relation_identity;
import bioseq.postgres.column.table_access;
import bioseq.postgres.storage.metadata; // column_table_row_count
import bioseq.postgres.storage.indexing.admin.index_build_runtime;
import bioseq.postgres.storage.indexing.admin.diagnostic_support;
import bioseq.postgres.storage.indexing.admin.index_sketch_backend_policy;
import bioseq.postgres.storage.indexing.admin.minimizer_kernel_selection;
import bioseq.postgres.storage.indexing.admin.scheme_untagged_fragment_guard;
import bioseq.postgres.storage.indexing.supported_indexes;
export import bioseq.postgres.storage.indexing.admin.rebuild.promoted_sequence;
import bioseq.postgres.storage.indexing.admin.rebuild.generation_route;
static_assert(__cplusplus >= 202302L, "C++23-or-later");

export namespace bioseq::storage::indexing {

/** Return whether a supplied compile-time registry contains an index name. */
template <class p_supported_indexes>
[[nodiscard]] inline bool registered_index_name(std::string_view name) {
    for (const auto& type : p_supported_indexes::list_types()) {
        if (type.first == name) {
            return true;
        }
    }
    return false;
}

/** Return the physical layout recorded by a supplied compile-time registry. */
template <class p_supported_indexes>
[[nodiscard]] inline sequence_index_layout registered_index_layout(std::string_view name) {
    return p_supported_indexes::layout_of(name);
}

/**
 * Run a build and close its storage on success On failure, give PostgreSQL
 * ownership of jrror cleanup where supported, or release temporary runs for
 * a C++ failure before rethrowing it.
 */
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

/// Own a packed sequence and its reserved memory during a parallel index build.
template <sequence_alphabet p_alphabet> struct resident_index_build_target {
    bioseq::storage::admitted_packed_sequence<p_alphabet> owner;
};

/// Copy a streamed target into reserved memory when CPU parallelism  is enabled
/// and the memory limit permits it. Return no value when copying is unnecessary
/// or unavailable, so the caller can keep, reading the sequence as a stream.
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

// Only scalar sequence-wide builds try copying the whole target into memory.
// Vulkan keeps reading the target in windows for GPU processing.
/// Rebuild a fragment-storage index, copying the source for scalar parallel work when possible.
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

/// Reject an inline source that would mix storage formats in a READY index.
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

/// Check  every non-null source for a PostgreSQL locator before rebuilding.
/// READY generations need these locators to verify source identity, and all
/// rows under one index name must use the same storage format.
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

    // Attempt every cleanup step, keeing the original failure if cleanup also fails.
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

/**
 * Rebuild the named index for all sequence columns in a Bioseq table
 * READY minimizer builds keep the previous generation until the new head is
 * published. Other layouts clear their old data before  building.
 *
 * @return Rows with a non-null sequence value, or the table row count for a table index.
 */
template <class p_supported_indexes>
inline std::int64_t update_index_impl(Oid rel_oid, std::string_view name) {
    if (!registered_index_name<p_supported_indexes>(name)) {
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
    const sequence_index_layout layout = registered_index_layout<p_supported_indexes>(name);
    const auto frozen_minimizer_algorithm =
        static_cast<bioseq::simd::minimizer_algorithm>(
            static_cast<int>(bioseq::aligner::settings::minimizer::algorithm.get_value()));
    const auto backend = resolve_index_sketch_backend(frozen_minimizer_algorithm);
    const bool is_minimizer_index =
        name == bioseq::indexing::k_minimizers_per_fragment_index_name ||
        name == bioseq::indexing::k_minimizers_per_sequence_index_name;
    const bool use_grouped_soa_generation =
        is_minimizer_index && bioseq::aligner::settings::grouped_soa_generation::enabled;
    if (is_minimizer_index &&
        frozen_minimizer_algorithm ==
            bioseq::simd::minimizer_algorithm::simd_minimap2_reference &&
        (!use_grouped_soa_generation || layout == sequence_index_layout::sequence_inline)) {
        bioseq::postgres::support::invoke_pg_as_cpp([] {
            ereport(ERROR,
                    (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                     errmsg("minimap2-reference index generation requires READY identity storage"),
                     errhint("Enable grouped-SoA generation for a fragmented minimizer index and "
                             "rebuild.")));
        });
        pg_unreachable();
    }
    const bioseq::minimizers::minimizer_kernels* minimizer_kernels = nullptr;
    bool minimizer_runtime_ready = false;
    const int configured_kmer_size = bioseq::aligner::settings::minimizer::kmer_size;
    const int configured_window_size = bioseq::aligner::settings::minimizer::window_size;
    const bool runtime_profile_fields_encodable =
        configured_kmer_size > 0 &&
        configured_kmer_size <= static_cast<int>((std::numeric_limits<std::uint16_t>::max)()) &&
        configured_window_size > 0 &&
        configured_window_size <= static_cast<int>((std::numeric_limits<std::uint16_t>::max)());
    bool registered_minimizer_profile = false;
    if (runtime_profile_fields_encodable) {
        const auto runtime_kmer_size = static_cast<std::uint16_t>(configured_kmer_size);
        const auto runtime_window_size = static_cast<std::uint16_t>(configured_window_size);
        for (const auto& candidate :
             bioseq::minimizers::registered_minimizer_runtime_profiles()) {
            if (bioseq::minimizers::find_minimizer_runtime_profile(
                    frozen_minimizer_algorithm,
                    candidate.alphabet_id,
                    runtime_kmer_size,
                    runtime_window_size,
                    candidate.minimum_position_log2,
                    candidate.packed_storage_bits) != nullptr) {
                registered_minimizer_profile = true;
                break;
            }
        }
    }
    if (use_grouped_soa_generation && !registered_minimizer_profile) {
        bioseq::postgres::support::invoke_pg_as_cpp(
            [configured_kmer_size, configured_window_size] {
                ereport(ERROR,
                        (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                         errmsg("READY minimizer generation profile is unavailable"),
                         errdetail("k=%d w=%d", configured_kmer_size, configured_window_size),
                         errhint("Use k/w values admitted by the generated profile registry; the "
                                 "exact source alphabet, packing, and physical capacity are "
                                 "validated before generation.")));
            });
        pg_unreachable();
    }
    const std::uint16_t frozen_kmer_size = static_cast<std::uint16_t>(configured_kmer_size);
    const std::uint16_t frozen_window_size = static_cast<std::uint16_t>(configured_window_size);
    if (is_minimizer_index) {
        // Validate the minimizer storage catalogs before allocating or writing a generation.
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
    bioseq_log<logging_level::debug>(
        "[bioseq] index: update_index name={} backend={} vulkan_enabled={} "
        "vulkan_gpu_index_build={} minimizer_algorithm={} grouped-SoA={}",
        name,
        bioseq::minimizers::minimizer_sketch_backend_name(backend),
        bioseq::aligner::settings::vulkan::enabled,
        bioseq::aligner::settings::vulkan::index_build::enabled,
        static_cast<int>(frozen_minimizer_algorithm),
        use_grouped_soa_generation);

    // Include every sequence column because mmsearch can target any of them.
    std::vector<std::int16_t> seq_attnos;
    const std::string seq_attno_sql =
        fmt::format("SELECT a.attnum, a.atttypmod FROM pg_attribute a JOIN pg_type t "
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
        std::array<std::int32_t, MaxTupleAttributeNumber> typmods{};
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
                const Datum typmod = SPI_getbinval(
                    SPI_tuptable->vals[row], SPI_tuptable->tupdesc, 2, &is_null);
                if (is_null) { state.null_row = row; break; }
                state.typmods[static_cast<std::size_t>(row)] = DatumGetInt32(typmod);
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

    // Reject declared CIGAR columns before kernel binding or GPU initialization.
    if (name != bioseq::indexing::k_symbol_counts_index_name) {
        for (std::size_t index = 0; index < state.row_count; ++index) {
            const auto typmod = state.typmods[index];
            if (typmod >= 0 && pgseq_is_cigar_alphabet(
                    compact_sequence_stdc_type_params::decode(typmod).alphabet_code)) {
                bioseq::postgres::support::invoke_pg_as_cpp([name] {
                    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("index %.*s is incompatible with CIGAR sequences",
                            static_cast<int>(name.size()), name.data()),
                        errhint("CIGAR sequences support the symbol_counts index.")));
                });
                pg_unreachable();
            }
        }
    }
    const MemoryContext parent_context = CurrentMemoryContext;
    MemoryContext work = bioseq::postgres::support::invoke_pg_as_cpp([parent_context] {
        return AllocSetContextCreate(parent_context, "bioseq_update_index", ALLOCSET_DEFAULT_SIZES);
    });
    p_supported_indexes idx;
    idx.apply_table_defaults();

    if (use_grouped_soa_generation) {
        try {
            verify_grouped_soa_rebuild_sources_are_locators(rel_oid, seq_attnos, name, work);
        } catch (...) {
            std::exception_ptr failure = std::current_exception();
            try {
                bioseq::postgres::support::invoke_pg_as_cpp([work] { MemoryContextDelete(work); });
            } catch (...) {
                // Keep the source error. Transaction abort will release the context if deletion failed.
            }
            std::rethrow_exception(failure);
        }
    }

    // Clear layouts that replace their stored data in place. READY minimizers
    // append a generation and replace its head, preserving the old generation
    // for  rollback and later cleanup. Dropping their table here would lose it.
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
            auto q = open_sequence_storage_handle(rel_oid,
                                                  rid.db_oid,
                                                  rid.tablespace_oid,
                                                  rid.relfilenode,
                                                  a,
                                                  name,
                                                  sequence_locator{},
                                                  0,
                                                  nullptr,
                                                  work);
            auto tb = open_table_storage_handle(
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
            auto tb = open_table_storage_handle(
                rel_oid, rid.db_oid, rid.tablespace_oid, rid.relfilenode, a, name, caches, work);
            run_pg_index_build_with_cleanup(
                tb, [&idx, &name, &tb] { idx.rebuild_one_table(name, tb); });
        }
        for (std::int16_t a : seq_attnos)
            mark_serialized_index_write_successful_pg(rel_oid, rid.relfilenode, a, name);
        caches->close_all(/*commit=*/false); // Close cached sequence relations before transaction-end checks.
                                             // These caches were used for reading.
        bioseq::storage::destroy_seq_caches(caches);
        MemoryContextDelete(work);
        return written;
    }

    // Build each non-null sequence column in every, visible row.
    Relation ur = nullptr;
    TupleDesc utd = nullptr;
    Snapshot snap = nullptr;
    column_read_state* rs = nullptr;
    MemoryContext inner = nullptr;
    bioseq::storage::pg_sequence_source_provider* source_provider = nullptr;
    // Read stored values through the column reader so large sequences remain
    // locator stubs. A nomal table scan may assemble the whole sequence into
    // a varlena and exceed MaxAllocSize. The locator lets each builder read
    // fragments as needed. The column row number is also  the index query key
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
        // The column reader can refresh its snapshot after flushing pending writes.
        // Bind the source provider to that snapshot so source checks and row reads agree.
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
            // Reuse the row context after releasing the previous row's sequence views.
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
                if (pgseq_is_cigar_alphabet(alpha)) {
                    if (name != bioseq::indexing::k_symbol_counts_index_name) {
                        bioseq::postgres::support::invoke_pg_as_cpp([name] {
                            ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                                errmsg("index %.*s is incompatible with CIGAR sequences",
                                    static_cast<int>(name.size()), name.data())));
                        });
                        pg_unreachable();
                    }
                    sequence_locator cigar_locator{};
                    Datum cigar_datum = PointerGetDatum(hdr);
                    if (is_locator_stub(hdr)) {
                        cigar_locator = decode_locator_from_stub(hdr);
                        cigar_datum = bioseq::storage::promote_pg_sequence_locator(
                            source_provider, seq_attno, cigar_datum, inner);
                    } else {
                        cigar_locator.logical_length = hdr->bp_size;
                        cigar_locator.bucket_index = static_cast<std::uint8_t>(
                            bioseq::storage::sequence_bucket_index<bioseq::storage::sequence_size_t, 8, 42>(
                                cigar_locator.logical_length));
                    }
                    auto store = open_sequence_storage_handle(rel_oid, rid.db_oid,
                        rid.tablespace_oid, rid.relfilenode, seq_attno, name,
                        cigar_locator, rowno, caches, work);
                    run_pg_index_build_with_cleanup(store, [&store, cigar_datum] {
                        const auto rebuilt = with_pg_cigar_sequence<std::expected<void, bioseq::cigar_error>>(
                            cigar_datum, [&store](auto& cigar_backend, auto cigar_view) {
                                auto cursor = cigar_backend.runs(cigar_view, 0u, cigar_backend.length(cigar_view));
                                return bioseq::indexing::symbol_counts_index<sequence_index_storage>::rebuild(store, cursor);
                            });
                        if (!rebuilt) bioseq::postgres::support::invoke_pg_as_cpp(
                            [error = rebuilt.error()] { pg_raise_cigar_error(error); });
                    });
                    any_col = true;
                    continue;
                }
                if (is_minimizer_index && !minimizer_runtime_ready) {
                    minimizer_kernels = bind_selected_minimizer_kernels_for_operation(
                        backend, "bioseq.update_index");
                    ensure_vulkan_index_build_runtime_ready(backend);
                    minimizer_runtime_ready = true;
                }
                if (is_minimizer_index && !use_grouped_soa_generation &&
                    backend == bioseq::minimizers::minimizer_sketch_backend::bounded_nthash_simd) {
                    reject_bounded_nthash_for_scheme_untagged_fragment_storage(
                        backend, "bioseq.update_index");
                }
                if (is_locator_stub(hdr)) {
                    const sequence_locator loc = decode_locator_from_stub(hdr);
                    const Datum promoted_datum = bioseq::storage::promote_pg_sequence_locator(
                        source_provider, seq_attno, PointerGetDatum(hdr), inner);
                    if (use_grouped_soa_generation &&
                        layout != sequence_index_layout::sequence_inline) {
                        const std::uint64_t algorithm_position_extent_bp =
                            name == bioseq::indexing::k_minimizers_per_fragment_index_name
                                ? bioseq::indexing::
                                      k_minimizer_fragment_algorithm_position_extent_bp
                                : loc.logical_length;
                        const auto algorithm_identity =
                            bioseq::indexing::
                                select_minimizer_generation_algorithm_identity_runtime(
                                    frozen_minimizer_algorithm,
                                    algorithm_position_extent_bp,
                                    alpha,
                                    static_cast<std::uint8_t>(bw * 8u),
                                    frozen_kmer_size,
                                    frozen_window_size);
                        if (!algorithm_identity) {
                            ready_minimizer_error_as_cpp(
                                ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
                                "READY minimizer generated algorithm identity is unavailable",
                                fmt::format("index={} backend={} alphabet={} storage_bits={} k={} w={}",
                                            name,
                                            bioseq::minimizers::minimizer_sketch_backend_name(
                                                backend),
                                            alpha,
                                            static_cast<unsigned>(bw) * 8u,
                                            frozen_kmer_size,
                                            frozen_window_size));
                        }
                        const auto generation_backend = resolve_index_sketch_backend(
                            algorithm_identity->effective_algorithm);
                        const auto generation_producer =
                            bioseq::indexing::minimizer_producer_backend_identity_for_backend(
                                generation_backend);
                        if (!bioseq::indexing::is_valid_minimizer_generation_algorithm_identity(
                                *algorithm_identity, generation_producer)) {
                            ready_minimizer_error_as_cpp(
                                ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
                                "READY minimizer effective producer identity is unavailable");
                        }
                        const bioseq::minimizers::minimizer_kernels* const generation_kernels =
                            bind_selected_minimizer_kernels_for_operation(
                                generation_backend, "bioseq.update_index READY generation");
                        if (!bioseq::indexing::minimizer_generation_kernel_binding_is_valid(
                                generation_backend, *algorithm_identity, generation_kernels)) {
                            ready_minimizer_error_as_cpp(
                                ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
                                "READY minimizer effective kernel binding is unavailable");
                        }
                        rebuild_detail::build_and_verify_locator_minimizer_generation(
                            rebuild_detail::locator_generation_rebuild_request{
                                .user_relation_oid = rel_oid,
                                .database_oid = rid.db_oid,
                                .tablespace_oid = rid.tablespace_oid,
                                .main_relfilenode = rid.relfilenode,
                                .column_attno = seq_attno,
                                .locator = loc,
                                .snapshot = rs->snapshot,
                                .caches = caches,
                                .row_context = inner,
                                .work_context = work,
                                .promoted_datum = promoted_datum,
                                .index_name = name,
                                .sequence_rowno = rowno,
                                .storage_width = bw,
                                .alphabet_code = alpha,
                                .backend = generation_backend,
                                .algorithm_identity = *algorithm_identity,
                                .kernels = generation_kernels});
                    } else {
                        pgseq_dispatch_biological_instance(
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
                             backend,
                             minimizer_kernels,
                             bw,
                             alpha]<class p_sequence_type>() {
                                using source_binding =
                                    bioseq::storage::postgres_lazy_sequence_binding<
                                        p_sequence_type>;
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
                                bioseq::postgres::support::invoke_pg_as_cpp(
                                    [promoted_datum,
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
                                             backend,
                                             minimizer_kernels,
                                             bw,
                                             alpha](auto lazy_view) {
                                                if (layout ==
                                                    sequence_index_layout::sequence_inline) {
                                                    auto store = open_sequence_storage_handle(
                                                        rel_oid,
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
                                                        [&idx, &name, &store, lazy_view] {
                                                            idx.rebuild_one_inline(
                                                                name, store, lazy_view);
                                                        });
                                                } else {
                                                    auto store = open_fragment_storage(
                                                        rel_oid,
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
                    }
                } else {
                    if (use_grouped_soa_generation) {
                        reject_grouped_soa_nonlocator_source_as_cpp(name);
                    }
                    pgseq_dispatch_biological_instance(
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
                                auto store = open_sequence_storage_handle(rel_oid,
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
                any_col = true; // Detoast copies belong to the row context.
                                // The next row reset, or final cleanup, releases them.
            }
            if (any_col)
                ++written;
        }
        // Release the final row's sequence handle before checking and closing the
        // source provider. Earlier handles were released by the next row reset.
        bioseq::postgres::support::invoke_pg_as_cpp([inner] { MemoryContextReset(inner); });
        bioseq::postgres::support::invoke_pg_as_cpp([source_provider] {
            bioseq::storage::close_pg_sequence_source_provider(source_provider, /*is_commit=*/true);
        });
        source_provider = nullptr;
    } catch (...) {
        row_walk_failure = std::current_exception();
    }

    // Attempt all cleanup  while keeping the first failure. Close fragment caches
    // before the column reader because they borrow its snapshot.
    const auto cleanup_leaf = [&row_walk_failure](auto&& function) {
        try {
            function();
        } catch (...) {
            if (row_walk_failure == nullptr) {
                row_walk_failure = std::current_exception();
            }
        }
    };
    // Delete row values before aborting the provider so their source references
    // are released aefore the provider closes.
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
        mark_serialized_index_write_successful_pg(rel_oid, rid.relfilenode, a, name);
    }
    return written;
}

/// Report an unsupported Exact SIMD build as a PostgreSQL error.
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

/// Map bounded-ntHash failures to PostgreSQL error codes.
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

/**
 * Run an index rebuild and translate exceptions into PostgreSQL errors after
 * C++ owners have been destroyed. Preserve PostgreSQL error details and report
 * Exact SIMD and bounded-ntHash failures with their specific error codes.
 *
 * @return The row count reported by the  rebuild.
 */
template <class p_supported_indexes>
[[nodiscard]] inline std::int64_t
update_index_pg_checked_for(Oid rel_oid, const char* name_data, std::size_t name_size) {
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
        return update_index_impl<p_supported_indexes>(rel_oid, name);
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
    // C++ build owners have been destroyed. Report the saved error, to PostgreSQL now.
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

[[nodiscard]] std::int64_t
update_index_pg_checked(Oid rel_oid, const char* name_data, std::size_t name_size) {
    return update_index_pg_checked_for<pg_supported_indexes>(rel_oid, name_data, name_size);
}

} // namespace bioseq::storage::indexing
