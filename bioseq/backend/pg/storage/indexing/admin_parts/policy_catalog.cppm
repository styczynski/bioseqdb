/**
 * @file policy_catalog.cppm
 * @brief Index policy, catalog and shared publication failure contracts.
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
export module bioseq.postgres.storage.indexing.admin:policy_catalog;
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

export namespace bioseq::storage::indexing {

using bioseq::indexing::sequence_index_layout;
using bioseq::indexing::index_update_policy;
using bioseq::indexing::index_sub_key_t;
using bioseq::indexing::k_directory_sub_key;
using bioseq::indexing::k_generation_head_sub_key;
using bioseq::indexing::k_temporary_run_sub_key_base;
using bioseq::indexing::derive_minimizer_position_extent_sub_key;

#ifdef BIOSEQ_TEST_INDEXES
/// A trivial table-chunk index compiled into the dylib only under
/// -DBIOSEQ_TEST_INDEXES. It gives table_index_storage's
/// for_each_sequence / write_to / index_table_shard rollover live integration
/// coverage. build() streams every row (columnar rowno) and persists the row
/// count as one int64 chunk. allows_update_on_insert=false (table-wide-never).
template <class p_storage>
struct test_table_count_index {
    static_assert(bioseq::indexing::is_index_chunked_storage_for<p_storage, sequence_index_layout::table_chunks>);
    static constexpr sequence_index_layout index_type = sequence_index_layout::table_chunks;
    static constexpr std::string_view  index_name = "test_table_rowcount";
    using storage_type = p_storage;
    static void build(p_storage& storage) {
        std::int64_t count = 0;
        storage.for_each_sequence([&count](auto& /*lazy*/, std::size_t /*rowno*/, std::size_t /*total*/) {
            ++count;
        });
        storage.resize(sizeof(std::int64_t));   // one chunk
        storage.template write_to<std::int64_t>(0,
            [&count](std::size_t c, std::int64_t* buf, std::size_t cap) -> std::size_t {
                DEBUG_ASSERT(c == 0 && cap == 1, "test_table_rowcount: single int64 chunk");
                buf[0] = count;
                return c;
            });
    }
    static void rebuild(p_storage& storage) { storage.clear(index_name); build(storage); }
};
#endif
}  // namespace bioseq::storage::indexing

#ifdef BIOSEQ_TEST_INDEXES
export namespace bioseq::indexing {
template <> struct index_descriptor<bioseq::storage::indexing::test_table_count_index> {
    static constexpr sequence_index_layout index_type = sequence_index_layout::table_chunks;
    static constexpr std::string_view  index_name = "test_table_rowcount";
    static constexpr bool allows_update_on_insert = false;
};
}  // namespace bioseq::indexing
#endif

export namespace bioseq::storage::indexing {

/// The single registry of every production index. Under
/// -DBIOSEQ_TEST_INDEXES the test-only table-chunk test_table_count_index is
/// appended so its for_each_sequence/write_to/rollover path gets live coverage.
using pg_supported_indexes = bioseq::indexing::supported_indexes<
    fragment_index_storage,
    sequence_index_storage,
    table_index_storage,
    bioseq::indexing::minimizers_per_fragment_index,     // CHUNKED_PER_FRAGMENT
    bioseq::indexing::direct_minimizer_bucket_index,   // CHUNKED_PER_FRAGMENT
    bioseq::indexing::symbol_counts_index               // INLINE_PER_SEQUENCE
#ifdef BIOSEQ_TEST_INDEXES
    , test_table_count_index                             // CHUNKED_PER_TABLE,    "test_table_rowcount"
#endif
    >;

// g_last_index_merge_run_count is defined in bioseq_storage_indexing_pg
// (same namespace, imported) so merge_runs can set it; referenced unqualified below.

// ---- policy catalog -------------------------------------------------------
/// One enabled and maintenance-policy row from the index policy catalog.
struct policy_row { std::string name; bool enabled; bool update_on_insert; };

/// Raise a PostgreSQL error for an invalid policy SPI stage or result row.
[[noreturn]] inline void table_policy_spi_error_as_cpp(
        const char* stage,
        int status,
        std::uint64_t row = (std::numeric_limits<std::uint64_t>::max)()) {
    bioseq::postgres::support::invoke_pg_as_cpp(
        [stage, status, row] {
            if (row == (std::numeric_limits<std::uint64_t>::max)()) {
                ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("could not load the index policy"),
                     errdetail("SPI stage=%s status=%d", stage, status)));
            } else {
                ereport(ERROR,
                    (errcode(ERRCODE_DATA_EXCEPTION),
                     errmsg("index policy row is invalid"),
                     errdetail("SPI stage=%s row=%llu",
                               stage,
                               static_cast<unsigned long long>(row))));
            }
            pg_unreachable();
        });
    pg_unreachable();
}

/// Load every policy override for one physical sequence column.
[[nodiscard]] inline std::vector<policy_row> load_table_policy(
        Oid user_relid,
        Oid relfn,
        std::int16_t attno) {
    const std::string sql = fmt::format(
        "SELECT index_name, enabled, update_on_insert "
        "FROM bioseq_internal.index_policy "
        "WHERE user_relid = {}::oid AND main_relfilenode = {}::oid "
        "AND column_attno = {}",
        static_cast<unsigned>(user_relid),
        static_cast<unsigned>(relfn),
        static_cast<int>(attno));

    std::vector<policy_row> out;
    bioseq::postgres::support::spi_session session;
    const int connect_status = session.connect();
    if (connect_status != SPI_OK_CONNECT) {
        table_policy_spi_error_as_cpp(
            "SPI_connect", connect_status);
    }

    const int execute_status = bioseq::postgres::support::invoke_pg_as_cpp(
        [&sql] {
            return SPI_execute(sql.c_str(), /*read_only=*/true, /*tcount=*/0);
        });
    if (execute_status != SPI_OK_SELECT || SPI_tuptable == nullptr) {
        table_policy_spi_error_as_cpp(
            "SPI_execute", execute_status);
    }

    const std::uint64_t row_count = SPI_processed;
    out.reserve(static_cast<std::size_t>(row_count));
    for (std::uint64_t row = 0; row < row_count; ++row) {
        bool enabled_is_null = false;
        bool update_is_null = false;
        const Datum enabled = bioseq::postgres::support::invoke_pg_as_cpp(
            [row, &enabled_is_null] {
                return SPI_getbinval(
                    SPI_tuptable->vals[row],
                    SPI_tuptable->tupdesc,
                    2,
                    &enabled_is_null);
            });
        const Datum update_on_insert = bioseq::postgres::support::invoke_pg_as_cpp(
            [row, &update_is_null] {
                return SPI_getbinval(
                    SPI_tuptable->vals[row],
                    SPI_tuptable->tupdesc,
                    3,
                    &update_is_null);
            });
        if (enabled_is_null || update_is_null) {
            table_policy_spi_error_as_cpp(
                "SPI_getbinval", execute_status, row);
        }

        char* name_data = bioseq::postgres::support::invoke_pg_as_cpp(
            [row] {
                return SPI_getvalue(
                    SPI_tuptable->vals[row],
                    SPI_tuptable->tupdesc,
                    1);
            });
        if (name_data == nullptr) {
            table_policy_spi_error_as_cpp(
                "SPI_getvalue", execute_status, row);
        }

        std::string name;
        try {
            name.assign(name_data);
        } catch (...) {
            bioseq::postgres::support::invoke_pg_as_cpp(
                [name_data] { pfree(name_data); });
            throw;
        }
        bioseq::postgres::support::invoke_pg_as_cpp(
            [name_data] { pfree(name_data); });
        out.push_back(policy_row{
            std::move(name),
            DatumGetBool(enabled),
            DatumGetBool(update_on_insert)});
    }

    const int finish_status = session.finish();
    if (finish_status != SPI_OK_FINISH) {
        table_policy_spi_error_as_cpp(
            "SPI_finish", finish_status);
    }
    return out;
}

// ---- age log + policy upsert ---------------------------------------------
/// Record the successful publication age for one persisted index.
inline void note_successful_write(Oid user_relid, Oid relfn, std::int16_t attno, std::string_view name) {
    static_cast<void>(upsert_metadata_age_log_pg(
        user_relid, relfn, attno, name));
}

/// Record successful INSERT maintenance for every enabled per-insert index.
inline void note_policy_on_insert_writes(
        Oid user_relid,
        Oid relfn,
        std::int16_t attno,
        const pg_supported_indexes& idx) {
    constexpr auto k_index_types = pg_supported_indexes::list_types();
    for (std::size_t i = 0; i < k_index_types.size(); ++i) {
        if (idx.enabled[i]
            && idx.policy[i] == index_update_policy::update_on_insert) {
            note_successful_write(
                user_relid, relfn, attno, k_index_types[i].first);
        }
    }
}

// Select the target index sketch backend from the minimizer producer itself.
// Persisted minimizer semantics do not depend on the later chaining/alignment
// choices: changing either stage must not silently relabel an explicitly
// selected exact SIMD or bounded_nthash_simd index as scalar.
// Defined before its first caller (index_build_on_insert); the spec placed it
// near layout_of_name, which is below index_build_on_insert.
/// Resolve the exact persisted minimizer producer selected by current GUCs.
[[nodiscard]] inline bioseq::minimizers::minimizer_sketch_backend
resolve_index_sketch_backend_from_gucs() noexcept
{
    using bioseq::simd::minimizer_algorithm;
    using bioseq::minimizers::minimizer_sketch_backend;
    if (bioseq::aligner::settings::vulkan::enabled
        && bioseq::aligner::settings::vulkan::index_build::enabled
        && static_cast<int>(bioseq::aligner::settings::minimizer::algorithm.get_value())
            == static_cast<int>(minimizer_algorithm::vulkan_minimap_integer)) {
        return minimizer_sketch_backend::vulkan_segment;
    }
    if (static_cast<int>(bioseq::aligner::settings::minimizer::algorithm.get_value())
            == static_cast<int>(minimizer_algorithm::two_stack_nthash)) {
        return minimizer_sketch_backend::exact_simd;
    }
    if (static_cast<int>(bioseq::aligner::settings::minimizer::algorithm.get_value())
            == static_cast<int>(minimizer_algorithm::bounded_nthash_simd)) {
        return minimizer_sketch_backend::bounded_nthash_simd;
    }
    return minimizer_sketch_backend::scalar;
}

// Prepare the shader provider and runtime cache before a Vulkan index build.
// PostgreSQL worker backends are forked after shared-library preload, so this
// path must establish the process-local provider and archive environment before
// submitting sketch work to the GPU runtime.
/// Prepare the backend-local Vulkan runtime for a selected GPU build.
inline void ensure_vulkan_index_build_runtime_ready(
    bioseq::minimizers::minimizer_sketch_backend backend)
{
    if (backend
        != bioseq::minimizers::minimizer_sketch_backend::vulkan_segment) {
        return;
    }
    (void)bioseq::postgres::scheduler::ensure_pg_vulkan_runtime_ready();
}

/**
 * Validate the immutable minimizer family selected by an owning operation.
 */
inline void require_selected_minimizer_kernels(
        bioseq::minimizers::minimizer_sketch_backend backend,
        const bioseq::minimizers::minimizer_kernels* kernels,
        std::string_view operation) {
    if (backend == bioseq::minimizers::minimizer_sketch_backend::exact_simd
        && (kernels == nullptr || kernels->two_stack_nthash == nullptr)) {
        throw bioseq::indexing::exact_simd_index_runtime_error{
            bioseq::indexing::exact_simd_index_status::invalid_state,
            operation};
    }
    if (backend
            == bioseq::minimizers::minimizer_sketch_backend::bounded_nthash_simd
        && (kernels == nullptr || kernels->bounded_nthash == nullptr)) {
        throw bioseq::indexing::packed_bounded_nthash_index_runtime_error{
            bioseq::indexing::packed_bounded_nthash_index_status::invalid_state,
            operation};
    }
}

/**
 * Bind the process-local table for one PostgreSQL maintenance operation.
 *
 * The cold plan chooses one runnable target once at the owning operation root.
 * The generated exact-target lookup is a direct table rebind, not a nested
 * Highway dispatch. scalar and Vulkan producers do not require this table.
 */
[[nodiscard]] inline const bioseq::minimizers::minimizer_kernels*
selected_minimizer_kernels_for_operation(
        bioseq::minimizers::minimizer_sketch_backend backend,
        std::string_view operation) {
    using backend_type = bioseq::minimizers::minimizer_sketch_backend;
    if (backend != backend_type::exact_simd
        && backend != backend_type::bounded_nthash_simd) {
        return nullptr;
    }
    const auto plan =
        bioseq::simd_dispatch::registered_highway_dispatch_plan();
    const auto& binding =
        bioseq::simd_dispatch::registered_highway_target_binding(
            plan.planned_target);
    const auto* kernels = &binding.kernels->minimizers;
    require_selected_minimizer_kernels(backend, kernels, operation);
    return kernels;
}

/// Scheme-untagged minimizer fragment and sequence rows do not carry a
/// minimizer-scheme identity.
/// Persisting bounded ntHash into that fragment layout would let a later
/// Exact reader (or the reverse) accept conflicting hashes under the same
/// index name. PostgreSQL-locator rebuilds use the scheme-tagged grouped-SoA
/// generation route. Only an inline scheme-untagged source reaches this
/// guard from update_index. The INSERT hook uses the same fragment layout and
/// keeps the guard until it can source a published PostgreSQL locator.
/// Silently substituting scalar or Exact is never permitted.
[[noreturn]] inline void reject_bounded_nthash_for_scheme_untagged_fragment_layout(
        bioseq::minimizers::minimizer_sketch_backend backend,
        std::string_view operation) {
    if (backend
        != bioseq::minimizers::minimizer_sketch_backend::
            bounded_nthash_simd) {
        bioseq_report<logging_level::error, logging_target::postgres>()
            .with_sql_error_code(ERRCODE_INTERNAL_ERROR)
            .with_message(
                "scheme-untagged bounded ntHash persistence guard received "
                "a non-bounded-ntHash backend");
        __builtin_unreachable();
    }
    bioseq::postgres::support::invoke_pg_as_cpp([operation] {
        ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("bounded_nthash_simd persistence requires the scheme-tagged "
                    "READY generation layout"),
             errdetail("%.*s selected scheme-untagged minimizer storage, whose payload "
                       "does not record a minimizer-scheme identity; "
                       "persistence is rejected without fallback",
                       static_cast<int>(operation.size()), operation.data()),
             errhint("Enable the controlled grouped-SoA persisted-generation "
                     "route for locator-backed bioseq.update_index, or select "
                     "Exact SIMD for the scheme-untagged index.")));
    });
    pg_unreachable();
}

// ---- INSERT-time build ----------------------------------------------------
/// Build every eligible per-insert index for one locator-backed sequence row.
inline void index_build_on_insert(Oid user_relid, std::int16_t attno, std::uint64_t rowno, const bioseq::pg_types::seq* hdr,
                                  const std::optional<sequence_locator>& loc_opt, MemoryContext mctx) {
    if (!bioseq::settings::index::insert_maintenance::enabled) return;
    if (!loc_opt.has_value()) return;
    const sequence_locator& loc = *loc_opt;
    const relation_identity rid = index_relation_identity(user_relid);
    auto policy_rows = load_table_policy(user_relid, rid.relfilenode, attno);
    pg_supported_indexes idx; idx.apply_table_defaults(); idx.merge_overrides(policy_rows);
    reset_last_minimizer_sequence_build_stats();
    if (bioseq::aligner::settings::grouped_soa_generation::enabled) {
        constexpr auto k_index_types = pg_supported_indexes::list_types();
        for (std::size_t i = 0; i < k_index_types.size(); ++i) {
            const bool minimizer =
                k_index_types[i].first == k_minimizers_per_fragment_index_name
                || k_index_types[i].first == k_minimizers_per_sequence_index_name;
            if (minimizer && idx.enabled[i]
                && idx.policy[i] == index_update_policy::update_on_insert) {
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [index_name = k_index_types[i].first] {
                    ereport(ERROR,
                        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                         errmsg("grouped-SoA persistence forbids INSERT-time "
                                "minimizer maintenance"),
                         errdetail("index %.*s would write the scheme-untagged layout "
                                   "before its PostgreSQL locator can be "
                                   "reopened by the grouped-SoA coordinator",
                                   static_cast<int>(index_name.size()),
                                   index_name.data()),
                         errhint("Set bioseq.index.insert_maintenance.enabled=off and "
                                 "publish the index with bioseq.update_index "
                                 "after the source row commits.")));
                });
                pg_unreachable();
            }
        }
    }
    const auto backend = resolve_index_sketch_backend_from_gucs();
    const bioseq::minimizers::minimizer_kernels* const minimizer_kernels =
        selected_minimizer_kernels_for_operation(
            backend, "bioseq INSERT index build");
    if (backend
        == bioseq::minimizers::minimizer_sketch_backend::
            bounded_nthash_simd) {
        constexpr auto k_index_types = pg_supported_indexes::list_types();
        for (std::size_t i = 0; i < k_index_types.size(); ++i) {
            if (k_index_types[i].second == sequence_index_layout::fragment_chunks
                && idx.enabled[i]
                && idx.policy[i] == index_update_policy::update_on_insert) {
                reject_bounded_nthash_for_scheme_untagged_fragment_layout(
                    backend, "bioseq INSERT index build");
            }
        }
    }
    ensure_vulkan_index_build_runtime_ready(backend);
    bioseq::pg_types::seq::dispatch_instance(
        hdr->alphabet_code,
        hdr->storage_unit_byte_width,
        [hdr, rid, attno, user_relid, loc, rowno, mctx, &idx,
         backend, minimizer_kernels]<class p_sequence_type>() {
        compact_sequence_range<p_sequence_type> rng{*hdr};
        auto lazy = bioseq::storage::make_resident_lazy_sequence(rng);
        const auto lazy_view =
            bioseq::storage::make_lazy_sequence_view(lazy);
        std::map<std::string_view, fragment_index_storage> fcache;
        std::map<std::string_view, sequence_index_storage> qcache;
        auto open_frag =
            [&fcache, rid, attno, user_relid, loc, rowno, mctx](
                    std::string_view name) -> fragment_index_storage& {
            if (auto it = fcache.find(name); it != fcache.end()) return it->second;
            acquire_index_advisory_xact_lock(rid.db_oid, rid.tablespace_oid, rid.relfilenode, attno, name);
            return fcache.emplace(name, open_fragment_storage(user_relid, rid.db_oid, rid.tablespace_oid, rid.relfilenode,
                                     attno, name, loc, rowno, nullptr, mctx)).first->second; };
        auto open_inl =
            [&qcache, rid, attno, user_relid, loc, rowno, mctx](
                    std::string_view name) -> sequence_index_storage& {
            if (auto it = qcache.find(name); it != qcache.end()) return it->second;
            acquire_index_advisory_xact_lock(rid.db_oid, rid.tablespace_oid, rid.relfilenode, attno, name);
            return qcache.emplace(name, open_sequence_storage(user_relid, rid.db_oid, rid.tablespace_oid, rid.relfilenode,
                                     attno, name, loc, rowno, nullptr, mctx)).first->second; };
        try {
            idx.build_fragmented<true>(
                open_frag, lazy_view, backend, minimizer_kernels,
                hdr->alphabet_code);
            idx.build_sequence<true>(open_inl, lazy_view);
            for (auto& [name, storage] : fcache) {
                (void)name;
                storage.close_checked();
            }
            for (auto& [name, storage] : qcache) {
                (void)name;
                storage.close_checked();
            }
        } catch (...) {
            for (auto& [name, storage] : fcache) {
                (void)name;
                storage.abandon_after_pg_error_noexcept();
            }
            for (auto& [name, storage] : qcache) {
                (void)name;
                storage.abandon_after_pg_error_noexcept();
            }
            throw;
        }
    });
    note_policy_on_insert_writes(user_relid, rid.relfilenode, attno, idx);
}

/// Per-row driver invoked by the TAM insert hooks: builds on-insert indexes for
/// every SEQ column. `detoasted_values` carries the ORIGINAL (pre-stub) payload.
inline void index_build_on_insert_columns(Oid user_relid, const std::vector<std::int16_t>& seq_attnos,
        const bool* tts_isnull, Datum* detoasted_values,
        const std::vector<std::optional<sequence_locator>>& per_col_locator, std::uint64_t rowno, MemoryContext mctx) {
    if (!bioseq::settings::index::insert_maintenance::enabled) return;
    for (std::size_t c = 0; c < seq_attnos.size() && c < per_col_locator.size(); ++c) {
        if (!per_col_locator[c].has_value()) continue;
        const std::int16_t attno = seq_attnos[c];
        if (tts_isnull[attno - 1]) continue;
        const bioseq::pg_types::seq* hdr = reinterpret_cast<const bioseq::pg_types::seq*>(DatumGetPointer(detoasted_values[attno - 1]));
        index_build_on_insert(user_relid, attno, rowno, hdr, per_col_locator[c], mctx);
    }
}

// ---- letter_to_rank -------------------------------------------------------
namespace detail {
constexpr std::size_t k_num_alphabets = static_cast<std::size_t>(pgseq_supported_alphabets::alphabet_count);
constexpr std::size_t k_char_domain   = 256;
/// Build an alphabet-indexed byte-to-rank lookup table.
inline auto build_letter_rank_table() {
    std::array<std::array<std::int8_t, k_char_domain>, k_num_alphabets> table{};
    for (auto& row : table) row.fill((std::int8_t)-1);
    for (std::uint8_t alphabet_code = 0;
         alphabet_code < k_num_alphabets;
         ++alphabet_code) {
        // Rank conversion depends only on the alphabet.  Dispatching a
        // fabricated storage width here violates bioseq::pg_types::seq's active width
        // policy in optimized PostgreSQL builds (which intentionally enable
        // only 8-byte storage units).
        pgseq_supported_alphabets::dispatch_alphabet_by_code(
            alphabet_code,
            [&table, alphabet_code]<class p_alphabet>() {
                for (int char_code = 0;
                     char_code < static_cast<int>(k_char_domain);
                     ++char_code) {
                    const char letter = static_cast<char>(char_code);
                    if (!seqan3::char_is_valid_for<p_alphabet>(letter)) continue;
                    p_alphabet symbol{};
                    symbol.assign_char(letter);
                    table[alphabet_code][static_cast<std::uint8_t>(char_code)] =
                        static_cast<std::int8_t>(symbol.to_rank());
                }
            });
    }
    return table;
}
/// Return the process-local immutable byte-to-rank lookup table.
inline const auto& letter_rank_table() { static const auto t = build_letter_rank_table(); return t; }
}  // namespace detail
/// Convert one encoded alphabet letter to its rank or return negative one.
[[nodiscard]] inline int letter_to_rank(std::uint8_t alphabet_code, char letter) {
    if (alphabet_code >= detail::k_num_alphabets) return -1;
    return static_cast<int>(detail::letter_rank_table()[alphabet_code][
        static_cast<std::uint8_t>(letter)]);
}

// ---- per-row index-present probe -----------------------------------------
/// Check the publication witness for one fragment index row.
[[nodiscard]] inline bool index_present(Oid user_relid, Oid db, Oid spc, Oid relfn, std::int16_t attno,
                                        std::string_view index_name, const sequence_locator& loc, std::int64_t row_key,
                                        seq_caches* caches, MemoryContext mctx) {
    auto s = open_fragment_storage(
        user_relid, db, spc, relfn, attno, index_name, loc,
        static_cast<std::uint64_t>(row_key), caches, mctx);
    // both layouts gate on the k_directory_sub_key witness written LAST.
    try {
        const bool present = s.number_of_chunks(k_directory_sub_key) >= 1;
        s.close_checked();
        return present;
    } catch (...) {
        s.abandon_after_pg_error_noexcept();
        throw;
    }
}

// ---- estimate_index_presence (planner cost bias only) ---------------------
/// Estimate table coverage from shard statistics without scanning payloads.
[[nodiscard]] inline float estimate_index_presence(Oid user_relid, double estimated_total_rows, std::string_view index_name) {
    if (estimated_total_rows <= 0) return 0.0f;
    const relation_identity rid = index_relation_identity(user_relid);
    // Probe the bucket-0 shard-0 shard's reltuples (free, no scan).
    const std::string nm = format_seqidx_name(index_name, rid.db_oid, rid.tablespace_oid, rid.relfilenode, 0, 0, 0);
    const Oid ns = get_namespace_oid("bioseq_internal", true);
    if (!OidIsValid(ns)) return 0.0f;
    const Oid rel = get_relname_relid(nm.c_str(), ns);
    if (!OidIsValid(rel)) return 0.0f;
    double shard_reltuples = 0.0;
    { Relation r = bioseq::postgres::support::try_open_postgres_relation(rel, AccessShareLock);
      if (r != nullptr) { shard_reltuples = r->rd_rel->reltuples; relation_close(r, AccessShareLock); } }
    constexpr double k_fragment_count_estimate = 64.0;  // coarse nominal
    double est_rows = shard_reltuples < 0 ? (double)0 : shard_reltuples;
    double distinct_seq_est = est_rows / k_fragment_count_estimate;
    float cov = (float)std::min(1.0, distinct_seq_est / estimated_total_rows);
    return cov < 0.0f ? 0.0f : cov;
}

/// Scope one test-only bounded-ntHash failure across publisher construction.
class scoped_forced_bounded_nthash_index_failure {
public:
    /// Arm the configured failure only after publication state exists.
    void arm_after_publisher_state_begin(
            bioseq::minimizers::minimizer_sketch_backend backend)
            noexcept {
#ifdef BIOSEQ_TEST_INDEXES
        if (static_cast<int>(bioseq::settings::testing::persisted_index::failure_mode.get_value()) == 1
            && backend == bioseq::minimizers::
                minimizer_sketch_backend::bounded_nthash_simd) {
            bioseq::indexing::arm_forced_packed_bounded_index_status_for_test(
                bioseq::indexing::packed_bounded_nthash_index_status::
                    unsupported_vector_shape);
            armed_ = true;
        }
#else
        (void)backend;
#endif
    }

    ~scoped_forced_bounded_nthash_index_failure() {
        if (armed_) {
            bioseq::indexing::
                disarm_forced_packed_bounded_index_status_for_test();
        }
    }

private:
    bool armed_ = false;
};


}  // namespace bioseq::storage::indexing
