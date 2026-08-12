/**
 * @file admin.cppm
 * @brief PostgreSQL registry instantiation, per-sequence storage factories,
 *        INSERT/admin build drivers, symbol-count reader glue, and the C++
 *        implementations behind the SQL surface.
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
export module bioseq.postgres.storage.indexing.admin;
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
export import bioseq.postgres.storage.indexing.handles;
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
struct policy_row { std::string name; bool enabled; bool update_on_insert; };

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
inline void note_successful_write(Oid user_relid, Oid relfn, std::int16_t attno, std::string_view name) {
    static_cast<void>(upsert_metadata_age_log_pg(
        user_relid, relfn, attno, name));
}

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
 * The cold plan chooses one runnable target once at the owning operation root;
 * the generated exact-target lookup is a direct table rebind, not a nested
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
/// generation route below; only an inline scheme-untagged source reaches this
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
        auto open_frag = [&](std::string_view name) -> fragment_index_storage& {
            if (auto it = fcache.find(name); it != fcache.end()) return it->second;
            acquire_index_advisory_xact_lock(rid.db_oid, rid.tablespace_oid, rid.relfilenode, attno, name);
            return fcache.emplace(name, open_fragment_storage(user_relid, rid.db_oid, rid.tablespace_oid, rid.relfilenode,
                                     attno, name, loc, rowno, nullptr, mctx)).first->second; };
        auto open_inl = [&](std::string_view name) -> sequence_index_storage& {
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
inline const auto& letter_rank_table() { static const auto t = build_letter_rank_table(); return t; }
}  // namespace detail
[[nodiscard]] inline int letter_to_rank(std::uint8_t alphabet_code, char letter) {
    if (alphabet_code >= detail::k_num_alphabets) return -1;
    return (int)detail::letter_rank_table()[alphabet_code][(std::uint8_t)letter];
}

// ---- per-row index-present probe -----------------------------------------
[[nodiscard]] inline bool index_present(Oid user_relid, Oid db, Oid spc, Oid relfn, std::int16_t attno,
                                        std::string_view index_name, const sequence_locator& loc, std::int64_t row_key,
                                        seq_caches* caches, MemoryContext mctx) {
    auto s = open_fragment_storage(user_relid, db, spc, relfn, attno, index_name, loc, (std::uint64_t)row_key, caches, mctx);
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

// ---- admin update_index ---------------------------------------------------
[[nodiscard]] inline bool name_is_registered(std::string_view name) {
    for (auto& t : pg_supported_indexes::list_types()) if (t.first == name) return true;
    return false;
}
[[nodiscard]] inline sequence_index_layout layout_of_name(std::string_view name) {
    return pg_supported_indexes::layout_of(name);
}

template <class p_storage, class p_build_function>
inline void run_pg_index_build_with_cleanup(p_storage& storage, p_build_function&& build_fn) {
    auto release_runs = [&storage]() noexcept {
        if constexpr (requires (p_storage& cleanup_storage) {
                { cleanup_storage.release_runs_noexcept() } noexcept -> std::same_as<void>;
            }) {
            storage.release_runs_noexcept();
        } else if constexpr (requires (p_storage& cleanup_storage) {
                { cleanup_storage.release_resident_runs_noexcept() } noexcept -> std::same_as<void>;
            }) {
            storage.release_resident_runs_noexcept();
        }
    };
    try {
        build_fn();
        if constexpr (requires (p_storage& completed_storage) {
                { completed_storage.close_checked() } -> std::same_as<void>;
            }) {
            storage.close_checked();
        }
    } catch (const bioseq::postgres::support::postgres_error&) {
        if constexpr (requires (p_storage& failed_storage) {
                { failed_storage.abandon_after_pg_error_noexcept() }
                    noexcept -> std::same_as<void>;
            }) {
            storage.abandon_after_pg_error_noexcept();
        }
        throw;
    } catch (...) {
        if constexpr (requires (p_storage& failed_storage) {
                { failed_storage.abandon_after_pg_error_noexcept() }
                    noexcept -> std::same_as<void>;
            }) {
            storage.abandon_after_pg_error_noexcept();
        } else {
            release_runs();
        }
        throw;
    }
}

template <sequence_alphabet p_alphabet>
struct resident_index_build_target {
    bioseq::storage::admitted_packed_sequence<p_alphabet> owner;
};

template <sequence_alphabet p_alphabet>
[[nodiscard]] std::optional<resident_index_build_target<p_alphabet>>
try_materialize_for_parallel_sequence_index_build(
        bioseq::storage::lazy_sequence_view<p_alphabet> seq,
        std::uint8_t requested_width,
        std::uint64_t rowno,
        std::string_view index_name) {
    bioseq::postgres::scheduler::backend_sync();
    auto& pool = bioseq::parallelism::global_pool::instance();
    if (!bioseq::parallelism::has_parallel_tasks(pool.get_mode())
        || pool.maximum_concurrency() <= 1) {
        return std::nullopt;
    }
    const auto access = bioseq::storage::plan_sequence_access(
        seq,
        bioseq::storage::sequence_access_requirement::contiguous_resident,
        0u,
        seq.bp_size(),
        requested_width);
    if (access.route
        == bioseq::storage::sequence_access_route::borrow_resident) {
        return std::nullopt;
    }
    constexpr std::uint64_t k_context_reserve_bytes = std::uint64_t{8} << 20;
    const std::uint64_t reserve_bytes = access.admission_bytes
            > (std::numeric_limits<std::uint64_t>::max)()
                - k_context_reserve_bytes
        ? (std::numeric_limits<std::uint64_t>::max)()
        : access.admission_bytes + k_context_reserve_bytes;
    auto permit =
        bioseq::parallelism::memory_admission_controller::instance().try_reserve(
            reserve_bytes);
    if (!permit) {
      bioseq_log<logging_level::debug>(
          "[bioseq] index: row={} index={} target_bp={} admitted_bytes={} "
          "not admitted for resident parallel build; using rank stream",
          static_cast<unsigned long long>(rowno), index_name,
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
        static_cast<unsigned long long>(rowno), index_name,
        static_cast<unsigned long long>(seq.bp_size()),
        static_cast<unsigned long long>(access.admission_bytes));
    return resident_index_build_target<p_alphabet>{std::move(owner)};
}

// Whole-target resident materialization is disabled under Vulkan: the goal is
// streaming fragmented GPU windows, not making the fragmented target
// contiguous on the host.
template <class p_supported_indexes, class p_storage, sequence_alphabet p_alphabet>
inline void rebuild_one_fragmented_maybe_resident(
        p_supported_indexes& idx,
        std::string_view name,
        p_storage& store,
        bioseq::storage::lazy_sequence_view<p_alphabet> sequence,
        std::uint8_t requested_width,
        std::uint8_t alphabet_code,
        std::uint64_t rowno,
        bioseq::minimizers::minimizer_sketch_backend backend,
        const bioseq::minimizers::minimizer_kernels* kernels) {
    if (backend == bioseq::minimizers::minimizer_sketch_backend::scalar
        && name == bioseq::indexing::k_minimizers_per_sequence_index_name) {
        if (auto resident =
                try_materialize_for_parallel_sequence_index_build(
                    sequence, requested_width, rowno, name);
            resident.has_value()) {
            idx.rebuild_one_fragmented(
                name, store, resident->owner.view(), backend, kernels,
                alphabet_code);
            return;
        }
    }
    idx.rebuild_one_fragmented(
        name, store, sequence, backend, kernels, alphabet_code);
}

/**
 * Adapt one source-domain-promoted Datum to the canonical lazy facade for the
 * duration of an index-build callback.  The read session owns every facade
 * adapter and is closed before the caller resets the row context containing
 * the expanded Datum.
 */
template <sequence_alphabet p_alphabet, class p_callback>
inline void with_promoted_pg_sequence_view(
        Datum promoted_datum,
        MemoryContext parent_context,
        p_callback&& callback) {
    const pg_sequence_descriptor descriptor =
        bioseq::postgres::support::invoke_pg_as_cpp([promoted_datum] {
            return classify_and_describe_sequence(promoted_datum);
        });
    std::exception_ptr callback_failure;
    pg_sequence_read_session* volatile session = nullptr;
    PG_TRY();
    {
        auto* local_session = begin_pg_sequence_read_session(parent_context);
        session = local_session;
        try {
            auto* owner = make_pg_sequence_call_owner<p_alphabet>(
                *local_session, promoted_datum, descriptor);
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
        abort_pg_sequence_read_session_noexcept(
            const_cast<pg_sequence_read_session*>(session));
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
            typename bioseq::storage::postgres_lazy_sequence_binding<p_sequence_range>::
                alphabet_type> sequence,
        bioseq::storage::postgres_lazy_sequence_binding<p_sequence_range> source_binding,
        bioseq::minimizers::minimizer_sketch_backend backend,
        const bioseq::minimizers::minimizer_kernels* kernels);

template <class p_sequence_range>
inline void build_and_verify_grouped_soa_minimizer_generation_pg(
        Oid user_relid,
        std::int16_t column_attno,
        std::string_view index_name,
        std::uint64_t sequence_rowno,
        fragment_index_storage& storage,
        bioseq::storage::lazy_sequence_view<
            typename bioseq::storage::postgres_lazy_sequence_binding<p_sequence_range>::
                alphabet_type> sequence,
        bioseq::storage::postgres_lazy_sequence_binding<p_sequence_range> source_binding,
        bioseq::minimizers::minimizer_sketch_backend backend,
        const bioseq::minimizers::minimizer_kernels* kernels);

[[noreturn]] inline void reject_grouped_soa_nonlocator_source_as_cpp(
        std::string_view index_name) {
    bioseq::postgres::support::invoke_pg_as_cpp([index_name] {
        ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("grouped-SoA persistence requires exclusively "
                    "PostgreSQL locator-backed sequence sources"),
             errdetail("index %.*s encountered an inline source and would "
                       "otherwise mix grouped-SoA and scheme-untagged payload rows under "
                       "one physical name",
                       static_cast<int>(index_name.size()), index_name.data()),
             errhint("Disable grouped-SoA persistence for inline-source rebuilds or "
                     "rewrite every source through the bioseq table AM.")));
    });
    pg_unreachable();
}

/// Fail before the destructive index clear if the grouped-SoA rebuild
/// cannot use one homogeneous PostgreSQL-locator source domain. The table AM
/// normally guarantees locator stubs; this scan makes that prerequisite an
/// enforced storage contract rather than an assumption about current writes.
inline void verify_grouped_soa_rebuild_sources_are_locators(
        Oid rel_oid,
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
        state.relation = bioseq::postgres::support::invoke_pg_as_cpp(
            [rel_oid] {
                return bioseq::postgres::support::try_open_postgres_relation(rel_oid, AccessShareLock);
            });
        if (state.relation == nullptr) {
            bioseq::postgres::support::invoke_pg_as_cpp([rel_oid] {
                ereport(ERROR,
                    (errcode(ERRCODE_UNDEFINED_TABLE),
                     errmsg("grouped-SoA source preflight relation is unavailable"),
                     errdetail("relation_oid=%u",
                               static_cast<unsigned>(rel_oid))));
            });
            pg_unreachable();
        }
        state.tuple_desc = bioseq::postgres::support::invoke_pg_as_cpp(
            [&state] { return RelationGetDescr(state.relation); });
        state.registered_snapshot = bioseq::postgres::support::invoke_pg_as_cpp([] {
            return RegisterSnapshot(GetTransactionSnapshot());
        });
        state.scan_context = bioseq::postgres::support::invoke_pg_as_cpp(
            [parent_context] {
                return AllocSetContextCreate(
                    parent_context, "bioseq_grouped_soa_source_preflight",
                    ALLOCSET_DEFAULT_SIZES);
            });
        state.row_context = bioseq::postgres::support::invoke_pg_as_cpp([&state] {
            return AllocSetContextCreate(
                state.scan_context,
                "bioseq_grouped_soa_source_preflight_row",
                ALLOCSET_DEFAULT_SIZES);
        });
        List* projected = bioseq::postgres::support::invoke_pg_as_cpp(
            [&state, seq_attnos] {
                const MemoryContext previous =
                    MemoryContextSwitchTo(state.scan_context);
                List* result = nullptr;
                for (const std::int16_t attno : seq_attnos) {
                    result = lappend_int(result, static_cast<int>(attno));
                }
                MemoryContextSwitchTo(previous);
                return result;
            });
        state.read_state = bioseq::postgres::support::invoke_pg_as_cpp(
            [&state, projected] {
                return begin_column_read(
                    state.relation, state.tuple_desc, projected, NIL,
                    state.scan_context, state.registered_snapshot,
                    /*randomAccess=*/false, nullptr);
            });
        std::vector<Datum> values(
            static_cast<std::size_t>(state.tuple_desc->natts));
        std::vector<char> null_buffer(
            static_cast<std::size_t>(state.tuple_desc->natts));
        bool* nulls = reinterpret_cast<bool*>(null_buffer.data());
        std::uint64_t rowno = 0;
        while (bioseq::postgres::support::invoke_pg_as_cpp(
                [&state, &values, nulls, &rowno] {
                    return read_next_column_row(
                        state.read_state, values.data(), nulls, &rowno);
                })) {
            bioseq::postgres::support::invoke_pg_as_cpp(
                [&state] { MemoryContextReset(state.row_context); });
            for (const std::int16_t attno : seq_attnos) {
                if (nulls[attno - 1]) {
                    continue;
                }
                bioseq::pg_types::seq* header = bioseq::postgres::support::invoke_pg_as_cpp(
                    [&state, &values, attno] {
                        const MemoryContext previous =
                            MemoryContextSwitchTo(state.row_context);
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
        column_read_state* read_state =
            std::exchange(state.read_state, nullptr);
        cleanup_leaf([read_state] {
            bioseq::postgres::support::invoke_pg_as_cpp(
                [read_state] { end_column_read(read_state); });
        });
    }
    if (state.registered_snapshot != nullptr) {
        Snapshot snapshot =
            std::exchange(state.registered_snapshot, nullptr);
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

inline std::int64_t update_index_impl(Oid rel_oid, std::string_view name) {
    if (!name_is_registered(name)) {
        bioseq::postgres::support::invoke_pg_as_cpp([name] {
            ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bioseq.update_index received an unknown index"),
                 errdetail("index_name=%.*s",
                           static_cast<int>(name.size()), name.data())));
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
        name == bioseq::indexing::k_minimizers_per_fragment_index_name
        || name == bioseq::indexing::k_minimizers_per_sequence_index_name;
    const bioseq::minimizers::minimizer_kernels* const minimizer_kernels =
        is_minimizer_index
            ? selected_minimizer_kernels_for_operation(
                  backend, "bioseq.update_index")
            : nullptr;
    const bool use_grouped_soa_generation = is_minimizer_index
        && bioseq::aligner::settings::grouped_soa_generation::enabled;
    if (is_minimizer_index) {
        // Cold fail-closed gate before generation-id allocation, relation/file
        // creation, payload writes, or descriptor/head publication.
        bioseq::storage::validate_minimizer_generation_storage_catalogs_pg();
    }
    reset_last_minimizer_sequence_build_stats();
    if (use_grouped_soa_generation
        && (bioseq::settings::index::async_persistence::enabled
            || bioseq::settings::index::recompute_async_persistence::enabled)) {
        bioseq::postgres::support::invoke_pg_as_cpp([] {
            ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("grouped-SoA persistence requires asynchronous "
                        "persistence to be disabled"),
                 errdetail("bioseq.index.async_persistence.enabled=%s, "
                           "bioseq.index.recompute_async_persistence.enabled=%s",
                           bioseq::settings::index::async_persistence::enabled ? "on" : "off",
                           bioseq::settings::index::recompute_async_persistence::enabled ? "on" : "off"),
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
    const std::string seq_attno_sql = fmt::format(
        "SELECT a.attnum FROM pg_attribute a JOIN pg_type t "
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
    bioseq::postgres::support::invoke_pg_as_cpp(
        [&state, seq_attno_sql_data] noexcept {
            state.connect_status = SPI_connect();
            if (state.connect_status != SPI_OK_CONNECT) return;
            state.execute_status = SPI_execute(
                seq_attno_sql_data, true, 0);
            state.row_count = SPI_processed;
            state.table_present = SPI_tuptable != nullptr;
            state.overflow = state.row_count > state.values.size();
            if (state.execute_status == SPI_OK_SELECT
                && state.table_present && !state.overflow) {
                for (std::uint64_t row = 0; row < state.row_count; ++row) {
                    bool is_null = false;
                    const Datum value = SPI_getbinval(
                        SPI_tuptable->vals[row], SPI_tuptable->tupdesc,
                        1, &is_null);
                    if (is_null) {
                        state.null_row = row;
                        break;
                    }
                    state.values[static_cast<std::size_t>(row)] =
                        DatumGetInt16(value);
                }
            }
            state.finish_status = SPI_finish();
        });
    if (state.connect_status != SPI_OK_CONNECT
        || state.execute_status != SPI_OK_SELECT
        || !state.table_present || state.overflow
        || state.null_row != (std::numeric_limits<std::uint64_t>::max)()
        || state.finish_status != SPI_OK_FINISH) {
        bioseq::postgres::support::invoke_pg_as_cpp([&state] {
            ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("bioseq.update_index sequence-column query failed"),
                 errdetail("SPI_connect_status=%d SPI_execute_status=%d "
                           "rows=%llu tuptable=%s overflow=%s null_row=%llu "
                           "SPI_finish_status=%d",
                           state.connect_status, state.execute_status,
                           static_cast<unsigned long long>(state.row_count),
                           state.table_present ? "present" : "null",
                           state.overflow ? "true" : "false",
                           static_cast<unsigned long long>(state.null_row),
                           state.finish_status)));
        });
        pg_unreachable();
    }
    seq_attnos.assign(
        state.values.begin(),
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
    MemoryContext work = bioseq::postgres::support::invoke_pg_as_cpp(
        [parent_context] {
            return AllocSetContextCreate(
                parent_context, "bioseq_update_index",
                ALLOCSET_DEFAULT_SIZES);
        });
    pg_supported_indexes idx; idx.apply_table_defaults();

    if (use_grouped_soa_generation) {
        try {
            verify_grouped_soa_rebuild_sources_are_locators(
                rel_oid, seq_attnos, name, work);
        } catch (...) {
            std::exception_ptr failure = std::current_exception();
            try {
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [work] { MemoryContextDelete(work); });
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
            acquire_index_advisory_xact_lock(rid.db_oid, rid.tablespace_oid, rid.relfilenode, a, name);
            auto f = open_fragment_storage(rel_oid, rid.db_oid, rid.tablespace_oid, rid.relfilenode, a, name, sequence_locator{}, 0, nullptr, work);
            auto q = open_sequence_storage(rel_oid, rid.db_oid, rid.tablespace_oid, rid.relfilenode, a, name, sequence_locator{}, 0, nullptr, work);
            auto tb = open_table_storage(rel_oid, rid.db_oid, rid.tablespace_oid, rid.relfilenode, a, name, nullptr, work);
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
        if (Relation ur = bioseq::postgres::support::try_open_postgres_relation(rel_oid, AccessShareLock); ur != nullptr) {
            written = static_cast<std::int64_t>(column_table_row_count(ur));
            relation_close(ur, AccessShareLock);
        }
        for (std::int16_t a : seq_attnos) {
            auto tb = open_table_storage(rel_oid, rid.db_oid, rid.tablespace_oid, rid.relfilenode, a, name, caches, work);
            run_pg_index_build_with_cleanup(tb, [&idx, &name, &tb] {
                idx.rebuild_one_table(name, tb);
            });
        }
        for (std::int16_t a : seq_attnos) note_successful_write(rel_oid, rid.relfilenode, a, name);
        caches->close_all(/*commit=*/false);   // close cached seq_sub read-state relations (else "resource was not closed" at commit)
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
                   errdetail("relation_oid=%u",
                             static_cast<unsigned>(rel_oid))));
          });
          pg_unreachable();
      }
      utd = bioseq::postgres::support::invoke_pg_as_cpp(
          [ur] { return RelationGetDescr(ur); });
      snap = bioseq::postgres::support::invoke_pg_as_cpp([] {
          return RegisterSnapshot(GetTransactionSnapshot());
      });
      List* projected = bioseq::postgres::support::invoke_pg_as_cpp(
          [work, &seq_attnos] {
              const MemoryContext previous = MemoryContextSwitchTo(work);
              List* result = nullptr;
              for (const std::int16_t attno : seq_attnos) {
                  result = lappend_int(result, static_cast<int>(attno));
              }
              MemoryContextSwitchTo(previous);
              return result;
          });
      rs = bioseq::postgres::support::invoke_pg_as_cpp(
          [ur, utd, projected, work, snap] {
              return begin_column_read(
                  ur, utd, projected, NIL, work, snap,
                  /*randomAccess=*/false, nullptr);
          });
      // BeginRead may flush pending writes and replace its snapshot with a
      // newly registered one. The provider and retained hash-major source
      // authorization must observe that same authoritative snapshot.
      source_provider =
          bioseq::storage::create_standalone_pg_sequence_source_provider(
              ur, rs->snapshot);
      inner = bioseq::postgres::support::invoke_pg_as_cpp([work] {
          return AllocSetContextCreate(
              work, "bioseq_update_index_inner_seq",
              ALLOCSET_DEFAULT_SIZES);
      });
      std::vector<Datum> vals(static_cast<std::size_t>(utd->natts));
      std::vector<char> nullbuf(static_cast<std::size_t>(utd->natts));
      Datum* valp = vals.data();
      bool* nulls = reinterpret_cast<bool*>(nullbuf.data());
      std::uint64_t rowno = 0;
      while (bioseq::postgres::support::invoke_pg_as_cpp(
              [rs, valp, nulls, &rowno] {
                  return read_next_column_row(
                      rs, valp, nulls, &rowno);
              })) {
        bioseq::postgres::support::invoke_pg_as_cpp(
            [inner] { MemoryContextReset(inner); });
        // Bound RSS to one row's decode plus one source fragment.
        bool any_col = false;
        for (std::int16_t seq_attno : seq_attnos) {
            if (nulls[seq_attno - 1]) continue;
            bioseq::pg_types::seq* hdr = bioseq::postgres::support::invoke_pg_as_cpp(
                [inner, valp, seq_attno] {
                    const MemoryContext previous =
                        MemoryContextSwitchTo(inner);
                    bioseq::pg_types::seq* result = reinterpret_cast<bioseq::pg_types::seq*>(
                        PG_DETOAST_DATUM(valp[seq_attno - 1]));
                    MemoryContextSwitchTo(previous);
                    return result;
                });
            const std::uint8_t alpha = hdr->alphabet_code;
            const std::uint8_t bw = logical_storage_unit_byte_width(hdr);
            if (is_minimizer_index && !use_grouped_soa_generation
                && backend == bioseq::minimizers::
                    minimizer_sketch_backend::bounded_nthash_simd) {
                reject_bounded_nthash_for_scheme_untagged_fragment_layout(
                    backend, "bioseq.update_index");
            }
            if (is_locator_stub(hdr)) {
                const sequence_locator loc = decode_locator_from_stub(hdr);
                const Datum promoted_datum =
                    bioseq::storage::promote_pg_sequence_locator(
                        source_provider, seq_attno, PointerGetDatum(hdr),
                        inner);
                bioseq::pg_types::seq::dispatch_instance(
                    alpha,
                    bw,
                    [rid, seq_attno, loc, rs, caches, inner,
                     promoted_datum, layout, rel_oid, name, rowno, work,
                     &idx, use_grouped_soa_generation, backend,
                     minimizer_kernels, bw,
                     alpha]<class p_sequence_type>() {
                    using source_binding =
                        bioseq::storage::postgres_lazy_sequence_binding<
                            p_sequence_type>;
                    const source_binding bound_source{
                        rid.db_oid, rid.tablespace_oid, rid.relfilenode,
                        seq_attno, loc,
                        source_binding::expected_last_fragment_padding(loc),
                        rs->snapshot, caches, inner};
                    bioseq::postgres::support::invoke_pg_as_cpp([
                        promoted_datum, inner, layout, rel_oid, rid,
                        seq_attno, &name, loc, rowno, caches, work, &idx,
                     use_grouped_soa_generation, bound_source, backend,
                        minimizer_kernels, bw, alpha] {
                      with_promoted_pg_sequence_view<
                          typename source_binding::alphabet_type>(
                          promoted_datum, inner,
                          [layout, rel_oid, rid, seq_attno, &name, loc, rowno,
                           caches, work, &idx, use_grouped_soa_generation,
                           bound_source, backend, minimizer_kernels,
                           bw, alpha](auto lazy_view){
                            if (layout == sequence_index_layout::sequence_inline) {
                                auto store = open_sequence_storage(rel_oid, rid.db_oid, rid.tablespace_oid, rid.relfilenode, seq_attno, name, loc, rowno, caches, work);
                                run_pg_index_build_with_cleanup(store, [&idx, &name, &store, lazy_view] {
                                    idx.rebuild_one_inline(
                                        name, store, lazy_view);
                                });
                            } else if (use_grouped_soa_generation) {
                                auto store = open_fragment_storage(rel_oid, rid.db_oid, rid.tablespace_oid, rid.relfilenode, seq_attno, name, loc, rowno, caches, work);
                                build_and_verify_hash_major_minimizer_generation_pg(
                                    rel_oid, seq_attno, name, rowno, store,
                                    lazy_view, bound_source, backend,
                                    minimizer_kernels);
                            } else {
                                auto store = open_fragment_storage(rel_oid, rid.db_oid, rid.tablespace_oid, rid.relfilenode, seq_attno, name, loc, rowno, caches, work);
                                run_pg_index_build_with_cleanup(store, [&idx, &name, &store, lazy_view, bw, alpha, rowno, backend, minimizer_kernels] {
                                    rebuild_one_fragmented_maybe_resident(
                                        idx, name, store, lazy_view, bw, alpha,
                                        rowno, backend, minimizer_kernels);
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
                    [hdr, layout, rel_oid, rid, seq_attno, name, rowno,
                     caches, work, &idx, bw, backend,
                     minimizer_kernels, alpha]<class p_sequence_type>() {
                    compact_sequence_range<p_sequence_type> rng{*hdr};
                    auto lazy = bioseq::storage::make_resident_lazy_sequence(rng);
                    const auto lazy_view =
                        bioseq::storage::make_lazy_sequence_view(lazy);
                    sequence_locator loc{}; loc.logical_length = rng.bp_size();
                    loc.bucket_index = (std::uint8_t)bioseq::storage::sequence_bucket_index<bioseq::storage::sequence_size_t,8,42>(loc.logical_length);
                    loc.shard_index = 0;
                    if (layout == sequence_index_layout::sequence_inline) {
                        auto store = open_sequence_storage(rel_oid, rid.db_oid, rid.tablespace_oid, rid.relfilenode, seq_attno, name, loc, rowno, caches, work);
                        run_pg_index_build_with_cleanup(store, [&idx, &name, &store, lazy_view] {
                            idx.rebuild_one_inline(
                                name, store, lazy_view);
                        });
                    } else {
                        auto store = open_fragment_storage(rel_oid, rid.db_oid, rid.tablespace_oid, rid.relfilenode, seq_attno, name, loc, rowno, caches, work);
                        run_pg_index_build_with_cleanup(store, [&idx, &name, &store, lazy_view, bw, alpha, rowno, backend, minimizer_kernels] {
                            rebuild_one_fragmented_maybe_resident(
                                idx, name, store, lazy_view, bw, alpha,
                                rowno, backend, minimizer_kernels);
                        });
                    }
                });
            }
            any_col = true;   // detoast copies live in `inner`, reclaimed by the per-row reset / final delete
        }
        if (any_col) ++written;
      }
      // Release the last expanded source handle before the provider performs
      // its checked commit sweep.  Earlier rows were released by their next
      // per-row reset.
      bioseq::postgres::support::invoke_pg_as_cpp(
          [inner] { MemoryContextReset(inner); });
      bioseq::postgres::support::invoke_pg_as_cpp([source_provider] {
          bioseq::storage::close_pg_sequence_source_provider(
              source_provider, /*is_commit=*/true);
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
            bioseq::storage::close_pg_sequence_source_provider(
                provider, /*is_commit=*/false);
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
            bioseq::postgres::support::invoke_pg_as_cpp([finished_caches] {
                bioseq::storage::destroy_seq_caches(finished_caches);
            });
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

[[noreturn]] inline void raise_exact_simd_index_pg_error(
		const char* operation,
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

class scoped_forced_bounded_nthash_index_failure {
public:
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
	bioseq::indexing::packed_bounded_nthash_index_status::unsupported_alphabet)
	== ERRCODE_FEATURE_NOT_SUPPORTED);
static_assert(packed_bounded_nthash_index_pg_errcode(
	bioseq::indexing::packed_bounded_nthash_index_status::unsupported_vector_shape)
	== ERRCODE_FEATURE_NOT_SUPPORTED);
static_assert(packed_bounded_nthash_index_pg_errcode(
	bioseq::indexing::packed_bounded_nthash_index_status::admission_denied)
	== ERRCODE_PROGRAM_LIMIT_EXCEEDED);
static_assert(packed_bounded_nthash_index_pg_errcode(
	bioseq::indexing::packed_bounded_nthash_index_status::input_too_large)
	== ERRCODE_PROGRAM_LIMIT_EXCEEDED);
static_assert(packed_bounded_nthash_index_pg_errcode(
	bioseq::indexing::packed_bounded_nthash_index_status::invalid_state)
	== ERRCODE_INTERNAL_ERROR);
static_assert(packed_bounded_nthash_index_pg_errcode(
	bioseq::indexing::packed_bounded_nthash_index_status::ok)
	== ERRCODE_INTERNAL_ERROR);

[[noreturn]] inline void raise_packed_bounded_nthash_index_pg_error(
		bioseq::indexing::packed_bounded_nthash_index_status status,
		const char* operation,
		const char* detail) {
	const int sqlerrcode = packed_bounded_nthash_index_pg_errcode(status);
	const std::string_view status_name =
		bioseq::indexing::packed_bounded_nthash_index_status_name(status);
	const char* message = sqlerrcode == ERRCODE_FEATURE_NOT_SUPPORTED
		? "Bounded ntHash SIMD minimizer index build is unsupported for this source"
		: sqlerrcode == ERRCODE_PROGRAM_LIMIT_EXCEEDED
			? "Bounded ntHash SIMD minimizer index build exceeded a resource limit"
			: sqlerrcode == ERRCODE_INTERNAL_ERROR
				? "Bounded ntHash SIMD minimizer index status contract was violated"
				: "Bounded ntHash SIMD minimizer index build rejected its request";
	ereport(ERROR,
		(errcode(sqlerrcode),
		 errmsg("%s", message),
		 errdetail_internal("%s: status=%.*s %s", operation,
			 static_cast<int>(status_name.size()), status_name.data(), detail),
			 errhint("Bounded ntHash SIMD has no scalar or Exact SIMD fallback; "
				 "use DNA4 with packed uint32/uint64 storage and W=10/K=15, "
				 "or increase the index memory limit when "
				 "status=admission_denied.")));
	pg_unreachable();
}

/// SQL-facing boundary: no C++ execution error may cross the PostgreSQL C ABI.
[[nodiscard]] inline std::int64_t update_index_pg_checked(
        Oid rel_oid,
        const char* name_data,
        std::size_t name_size) {
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
		strlcpy(
			detail.data(),
			err.what() == nullptr ? "Exact SIMD error without detail" : err.what(),
			detail.size());
		exact_simd_error = true;
	} catch (const bioseq::indexing::packed_bounded_nthash_index_runtime_error& err) {
		strlcpy(
			detail.data(),
			err.what() == nullptr
				? "Bounded ntHash SIMD error without detail" : err.what(),
			detail.size());
		packed_bounded_status = err.status();
		packed_bounded_error = true;
	} catch (const std::exception& error) {
		strlcpy(
			detail.data(),
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
		raise_exact_simd_index_pg_error(
			"bioseq.update_index", detail.data());
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
[[noreturn]] inline void load_index_spi_error_as_cpp(
        std::string_view operation,
        std::string_view detail) {
    bioseq::postgres::support::invoke_pg_as_cpp([operation, detail] {
        ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("bioseq.load post-load index maintenance failed"),
             errdetail("operation=%.*s %.*s",
                       static_cast<int>(operation.size()), operation.data(),
                       static_cast<int>(detail.size()), detail.data())));
    });
    pg_unreachable();
}

template <class p_decoder>
[[nodiscard]] inline auto strict_spi_select_one(
        const std::string& sql,
        bool read_only,
        std::string_view operation,
        p_decoder&& decode) -> std::invoke_result_t<p_decoder&, Datum> {
    using result_type = std::invoke_result_t<p_decoder&, Datum>;
    static_assert(!std::is_reference_v<result_type>);
    static_assert(std::is_trivially_copyable_v<result_type>);
    static_assert(std::is_default_constructible_v<result_type>);
    static_assert(std::is_nothrow_invocable_r_v<result_type, p_decoder&, Datum>,
        "SPI scalar decoders run before SPI_finish and must not throw");
    struct spi_scalar_state {
        int connect_status = 0;
        int execute_status = 0;
        int finish_status = 0;
        std::uint64_t row_count = 0;
        bool table_present = false;
        bool is_null = true;
        result_type result{};
    } state{};
    const char* sql_data = sql.c_str();
    bioseq::postgres::support::invoke_pg_as_cpp(
        [&state, sql_data, read_only, &decode] noexcept {
            state.connect_status = SPI_connect();
            if (state.connect_status != SPI_OK_CONNECT) return;
            state.execute_status = SPI_execute(sql_data, read_only, 0);
            state.row_count = SPI_processed;
            state.table_present = SPI_tuptable != nullptr;
            if (state.execute_status == SPI_OK_SELECT
                && state.row_count == 1 && state.table_present) {
                const Datum datum = SPI_getbinval(
                    SPI_tuptable->vals[0], SPI_tuptable->tupdesc,
                    1, &state.is_null);
                if (!state.is_null) state.result = decode(datum);
            }
            state.finish_status = SPI_finish();
        });
    if (state.connect_status != SPI_OK_CONNECT
        || state.execute_status != SPI_OK_SELECT
        || state.row_count != 1 || !state.table_present
        || state.is_null || state.finish_status != SPI_OK_FINISH) {
        load_index_spi_error_as_cpp(
            operation,
            fmt::format(
                "SPI_connect_status={} SPI_execute_status={} rows={} "
                "tuptable={} result_null={} SPI_finish_status={}",
                state.connect_status, state.execute_status, state.row_count,
                state.table_present ? "present" : "null",
                state.is_null ? "true" : "false", state.finish_status));
    }
    return state.result;
}

[[nodiscard]] inline std::vector<policy_row>
load_table_policy_strict_for_post_load(
        Oid user_relid,
        Oid relfilenode,
        std::int16_t attno) {
    constexpr std::size_t k_max_policy_rows =
        pg_supported_indexes::list_types().size();
    constexpr auto k_registered_types = pg_supported_indexes::list_types();
    struct raw_policy_row {
        std::uint32_t registry_ordinal = 0;
        bool enabled = false;
        bool update_on_insert = false;
    };
    struct spi_policy_state {
        int connect_status = 0;
        int execute_status = 0;
        int finish_status = 0;
        std::uint64_t row_count = 0;
        std::uint64_t matched_row_count = 0;
        std::uint64_t invalid_row =
            (std::numeric_limits<std::uint64_t>::max)();
        bool table_present = false;
        bool overflow = false;
        std::array<raw_policy_row, k_max_policy_rows> rows{};
    } state{};
    const std::string sql = fmt::format(
        "SELECT index_name,enabled,update_on_insert "
        "FROM bioseq_internal.index_policy "
        "WHERE user_relid={}::oid AND main_relfilenode={}::oid "
        "AND column_attno={} ORDER BY index_name",
        static_cast<unsigned>(user_relid),
        static_cast<unsigned>(relfilenode),
        static_cast<int>(attno));
    const char* sql_data = sql.c_str();
    bioseq::postgres::support::invoke_pg_as_cpp(
        [&state, &k_registered_types, sql_data] noexcept {
            state.connect_status = SPI_connect();
            if (state.connect_status != SPI_OK_CONNECT) return;
            state.execute_status = SPI_execute(sql_data, true, 0);
            state.row_count = SPI_processed;
            state.table_present = SPI_tuptable != nullptr;
            if (state.execute_status == SPI_OK_SELECT
                && state.table_present) {
                for (std::uint64_t row = 0; row < state.row_count; ++row) {
                bool name_null = false;
                bool enabled_null = false;
                bool update_null = false;
                const Datum name_datum = SPI_getbinval(
                    SPI_tuptable->vals[row], SPI_tuptable->tupdesc,
                    1, &name_null);
                const Datum enabled_datum = SPI_getbinval(
                    SPI_tuptable->vals[row], SPI_tuptable->tupdesc,
                    2, &enabled_null);
                const Datum update_datum = SPI_getbinval(
                    SPI_tuptable->vals[row], SPI_tuptable->tupdesc,
                    3, &update_null);
                if (name_null || enabled_null || update_null) {
                    state.invalid_row = row;
                    break;
                }
                text* name_text = DatumGetTextPP(name_datum);
                const std::size_t name_length =
                    static_cast<std::size_t>(VARSIZE_ANY_EXHDR(name_text));
                std::size_t registry_ordinal = k_registered_types.size();
                for (std::size_t candidate = 0;
                     candidate < k_registered_types.size(); ++candidate) {
                    const std::string_view registered_name =
                        k_registered_types[candidate].first;
                    if (name_length == registered_name.size()
                        && std::memcmp(
                            VARDATA_ANY(name_text), registered_name.data(),
                            name_length) == 0) {
                        registry_ordinal = candidate;
                        break;
                    }
                }
                if (registry_ordinal == k_registered_types.size()) continue;
                if (state.matched_row_count >= state.rows.size()) {
                    state.overflow = true;
                    break;
                }
                raw_policy_row& decoded = state.rows[static_cast<std::size_t>(
                    state.matched_row_count++)];
                decoded.registry_ordinal =
                    static_cast<std::uint32_t>(registry_ordinal);
                decoded.enabled = DatumGetBool(enabled_datum);
                decoded.update_on_insert = DatumGetBool(update_datum);
                }
            }
            state.finish_status = SPI_finish();
        });
    if (state.connect_status != SPI_OK_CONNECT
        || state.execute_status != SPI_OK_SELECT
        || !state.table_present || state.overflow
        || state.invalid_row != (std::numeric_limits<std::uint64_t>::max)()
        || state.finish_status != SPI_OK_FINISH) {
        load_index_spi_error_as_cpp(
            "load policy query",
            fmt::format(
                "SPI_connect_status={} SPI_execute_status={} rows={} "
                "matched_rows={} tuptable={} overflow={} invalid_row={} "
                "SPI_finish_status={}",
                state.connect_status, state.execute_status, state.row_count,
                state.matched_row_count,
                state.table_present ? "present" : "null",
                state.overflow ? "true" : "false", state.invalid_row,
                state.finish_status));
    }
    std::vector<policy_row> result;
    result.reserve(static_cast<std::size_t>(state.matched_row_count));
    for (std::size_t row = 0;
         row < static_cast<std::size_t>(state.matched_row_count); ++row) {
        const std::size_t registry_ordinal =
            state.rows[row].registry_ordinal;
        result.push_back(policy_row{
            std::string{k_registered_types[registry_ordinal].first},
            state.rows[row].enabled,
            state.rows[row].update_on_insert});
    }
    return result;
}

[[nodiscard]] inline std::int64_t build_policy_update_on_insert_indexes(Oid main_relid) {
    const relation_identity rid = index_relation_identity(main_relid);
    pg_supported_indexes idx;
    idx.apply_table_defaults();

    // Overlay the persisted per-table policy. set_index_policy_impl stamps every
    // seq column identically, so the first seq column is a faithful table-level
    // proxy; if none is found the registry defaults stand.
    const std::string attno_sql = fmt::format(
        "SELECT a.attnum FROM pg_attribute a JOIN pg_type t "
        "ON a.atttypid=t.oid JOIN pg_namespace n ON t.typnamespace=n.oid "
        "WHERE a.attrelid={}::oid AND t.typname='seq' "
        "AND n.nspname IN ('bioseq','bioseq_internal') "
        "AND a.attnum>0 AND NOT a.attisdropped "
        "ORDER BY a.attnum LIMIT 1",
        static_cast<unsigned>(main_relid));
    const std::int16_t first_seq_attno = strict_spi_select_one(
        attno_sql, /*read_only=*/true, "first sequence column",
        [](Datum value) noexcept { return DatumGetInt16(value); });
    if (first_seq_attno <= 0) {
        load_index_spi_error_as_cpp(
            "first sequence column", "attribute number is not positive");
    }
    idx.merge_overrides(load_table_policy_strict_for_post_load(
        main_relid, rid.relfilenode, first_seq_attno));

    constexpr auto k_index_types = pg_supported_indexes::list_types();
    std::int64_t total = 0;
    for (std::size_t i = 0; i < k_index_types.size(); ++i) {
        if (!idx.enabled[i]) continue;
        if (idx.policy[i] != index_update_policy::update_on_insert) continue;
        // Invoke the index build through the SQL executor (SPI) rather
        // than calling update_index_impl() directly. The direct call ran the
        // builder inside bioseq.load's own executor frame; the relcache/typcache
        // churn from creating the seqidx sidecar relations (CREATE TABLE ...
        // bioseq.seq via internal subtransactions) then leaked into the next
        // command in the same backend (fetch_att "unsupported byval length: -1"
        // / custom-scan SIGSEGV). Routing through SPI gives the build its own
        // portal/executor frame with clean snapshot + invalidation handling —
        // exactly what a separate `bioseq.update_index` statement gets, which is
        // why the manual reindex never left residue.
        const std::string index_name{k_index_types[i].first};
        const char* quoted_name = bioseq::postgres::support::invoke_pg_as_cpp(
            [&index_name] {
                return quote_literal_cstr(index_name.c_str());
            });
        const std::string sql = fmt::format(
            "SELECT bioseq.update_index({}::oid::regclass, {})",
            static_cast<unsigned>(main_relid),
            quoted_name);
        const std::int64_t written = strict_spi_select_one(
            sql, /*read_only=*/false,
            fmt::format("bioseq.update_index({})", index_name),
            [](Datum value) noexcept { return DatumGetInt64(value); });
        if (written < 0
            || written > (std::numeric_limits<std::int64_t>::max)() - total) {
            load_index_spi_error_as_cpp(
                "bioseq.update_index result",
                fmt::format("index={} count={} accumulated={}",
                            index_name, written, total));
        }
        total += written;
    }
    return total;
}

// ---- SQL helper implementations ------------------------------------------

/// Read-only setup/post-measurement inspection returned by the narrow SQL admin
/// bridge. The authoritative source locator is derived from the visible
/// Bioseq row; callers never supply a source identity or storage-contract
/// digest.
struct ready_minimizer_generation_inspection {
    ready_minimizer_generation_identity identity{};
    minimizer_generation_source_locator source{};
    minimizer_generation_manifest manifest{};
    std::uint64_t source_content_checksum = 0;
    Oid database_oid = InvalidOid;
    Oid main_relfilenode = InvalidOid;
    std::int16_t column_attno = 0;
};

/// One fully authenticated hash-major logical-stream receipt.  The descriptor
/// fields are publication authority; the exact_catalog_* fields are recomputed
/// by a bounded, row-aligned reread under the same registered MVCC snapshot.
struct hash_major_minimizer_generation_stream_inspection {
    bioseq::indexing::hash_major_minimizer_stream_kind kind =
        bioseq::indexing::hash_major_minimizer_stream_kind::directory;
    std::uint64_t element_count = 0;
    std::uint64_t logical_bytes = 0;
    std::uint64_t row_count = 0;
    std::uint64_t stream_crc64 = 0;
    std::uint64_t exact_catalog_rows = 0;
    std::uint64_t exact_catalog_bytes = 0;
    std::uint64_t exact_catalog_stream_crc64 = 0;
};

/// Separate hash-major restart/audit surface.  This must not be folded into the
/// grouped-SoA inspector: the two sentinels have different identity widths,
/// metadata sizes, and stream equations.
struct hash_major_minimizer_generation_inspection {
    Oid database_oid = InvalidOid;
    Oid main_relfilenode = InvalidOid;
    std::int16_t column_attno = 0;
    std::uint64_t query_key = 0;
    std::array<char, NAMEDATALEN> table_name{};
    std::array<char, NAMEDATALEN> index_name{};
    minimizer_generation_source_locator source{};
    std::uint64_t source_content_crc64 = 0;
    std::array<std::byte, 16> source_identity{};
    bioseq::indexing::hash_major_minimizer_head head{};
    bioseq::indexing::hash_major_minimizer_manifest manifest{};
    bioseq::indexing::encoded_hash_major_minimizer_head encoded_head{};
    bioseq::indexing::encoded_hash_major_minimizer_manifest encoded_manifest{};
    std::uint64_t head_rows = 0;
    std::uint64_t head_bytes = 0;
    std::uint64_t manifest_rows = 0;
    std::uint64_t manifest_bytes = 0;
    std::array<hash_major_minimizer_generation_stream_inspection, 3> streams{};
};

[[nodiscard]] inline std::int16_t
single_seq_attno_for_inspection(Oid rel_oid);

/// The execution-receipt alternative is the tag. There is deliberately no independent
/// mutable index-kind enum that a caller could relabel after an encoder returns.
using ready_minimizer_generation_execution_receipt = std::variant<
    std::monostate,
    bioseq::indexing::minimizer_fragment_generation_execution_receipt,
    bioseq::indexing::minimizer_sequence_generation_execution_receipt>;

enum class ready_minimizer_generation_index_kind : std::uint8_t {
    fragment,
    sequence,
};

/// Validated result of the production minimizer generation coordinator.
/// Metadata age is deliberately absent: callers own that update and may perform
/// it only after this function returns a published result.
class ready_minimizer_generation_publication {
private:
    template <class p_sequence_range>
    friend ready_minimizer_generation_publication
    build_and_publish_grouped_soa_minimizer_generation_pg(
        Oid,
        std::int16_t,
        std::string_view,
        std::uint64_t,
        fragment_index_storage&,
        bioseq::storage::lazy_sequence_view<
            typename bioseq::storage::postgres_lazy_sequence_binding<p_sequence_range>::
                alphabet_type>,
        bioseq::storage::postgres_lazy_sequence_binding<p_sequence_range>,
        bioseq::minimizers::minimizer_sketch_backend,
        const bioseq::minimizers::minimizer_kernels*);

    ready_minimizer_generation_index_kind index_kind_ =
        ready_minimizer_generation_index_kind::fragment;
    bioseq::indexing::minimizer_generation_publication_binding binding_{};
    std::uint64_t control_count_ = 0;
    std::uint64_t position_count_ = 0;
    std::uint64_t writer_rows_ = 0;
    std::uint64_t writer_batches_ = 0;
    bioseq::minimizers::minimizer_sketch_backend selected_backend_ =
        bioseq::minimizers::minimizer_sketch_backend::scalar;
    ready_minimizer_generation_execution_receipt execution_{};
    std::optional<std::uint64_t> replaced_generation_id_{};
    std::uint64_t minimizer_sequence_spill_generation_token_ = 0;
    std::uint64_t minimizer_sequence_spill_mutation_before_release_ = 0;
    std::uint64_t minimizer_sequence_spill_mutation_after_release_ = 0;
    bool minimizer_sequence_spill_resources_released_ = false;
    bool published_ = false;

    ready_minimizer_generation_publication(
            ready_minimizer_generation_index_kind index_kind,
            bioseq::indexing::minimizer_generation_publication_binding binding,
            std::uint64_t control_count,
            std::uint64_t position_count,
            std::uint64_t writer_rows,
            std::uint64_t writer_batches,
            bioseq::minimizers::minimizer_sketch_backend backend,
            ready_minimizer_generation_execution_receipt execution,
            std::optional<std::uint64_t> replaced_generation_id,
            std::uint64_t minimizer_sequence_spill_generation_token,
            std::uint64_t minimizer_sequence_spill_mutation_before_release,
            std::uint64_t minimizer_sequence_spill_mutation_after_release,
            bool minimizer_sequence_spill_resources_released) noexcept
        : index_kind_(index_kind),
          binding_(binding),
          control_count_(control_count),
          position_count_(position_count),
          writer_rows_(writer_rows),
          writer_batches_(writer_batches),
          selected_backend_(backend),
          execution_(std::move(execution)),
          replaced_generation_id_(replaced_generation_id),
          minimizer_sequence_spill_generation_token_(
              minimizer_sequence_spill_generation_token),
          minimizer_sequence_spill_mutation_before_release_(
              minimizer_sequence_spill_mutation_before_release),
          minimizer_sequence_spill_mutation_after_release_(
              minimizer_sequence_spill_mutation_after_release),
          minimizer_sequence_spill_resources_released_(
              minimizer_sequence_spill_resources_released),
          published_(true) {}

public:
    ready_minimizer_generation_publication() = delete;
    ready_minimizer_generation_publication(
        const ready_minimizer_generation_publication&) = default;
    ready_minimizer_generation_publication(
        ready_minimizer_generation_publication&&) noexcept = default;
    ready_minimizer_generation_publication& operator=(
        const ready_minimizer_generation_publication&) = delete;
    ready_minimizer_generation_publication& operator=(
        ready_minimizer_generation_publication&&) noexcept = delete;

    [[nodiscard]] ready_minimizer_generation_index_kind
    index_kind() const noexcept { return index_kind_; }
    [[nodiscard]] std::uint64_t generation_id() const noexcept {
        return binding_.generation_id;
    }
    [[nodiscard]] std::uint64_t source_identity() const noexcept {
        return binding_.source_identity;
    }
    [[nodiscard]] std::uint64_t source_content_checksum() const noexcept {
        return binding_.source_content_checksum;
    }
    [[nodiscard]] std::uint64_t storage_contract_checksum() const noexcept {
        return binding_.storage_contract_checksum;
    }
    [[nodiscard]] std::uint64_t control_count() const noexcept {
        return control_count_;
    }
    [[nodiscard]] std::uint64_t position_count() const noexcept {
        return position_count_;
    }
    [[nodiscard]] std::uint64_t writer_rows() const noexcept {
        return writer_rows_;
    }
    [[nodiscard]] std::uint64_t writer_batches() const noexcept {
        return writer_batches_;
    }
    [[nodiscard]] bioseq::minimizers::minimizer_sketch_backend
    selected_backend() const noexcept { return selected_backend_; }
    [[nodiscard]] const ready_minimizer_generation_execution_receipt&
    execution() const noexcept { return execution_; }
    [[nodiscard]] std::optional<std::uint64_t>
    replaced_generation_id() const noexcept {
        return replaced_generation_id_;
    }
    [[nodiscard]] std::uint64_t
    minimizer_sequence_spill_generation_token() const noexcept {
        return minimizer_sequence_spill_generation_token_;
    }
    [[nodiscard]] std::uint64_t
    minimizer_sequence_spill_mutation_before_release() const noexcept {
        return minimizer_sequence_spill_mutation_before_release_;
    }
    [[nodiscard]] std::uint64_t
    minimizer_sequence_spill_mutation_after_release() const noexcept {
        return minimizer_sequence_spill_mutation_after_release_;
    }
    [[nodiscard]] bool minimizer_sequence_spill_resources_released() const noexcept {
        return minimizer_sequence_spill_resources_released_;
    }
    [[nodiscard]] bool published() const noexcept { return published_; }

    [[nodiscard]] bool execution_accepted() const noexcept {
        if (!published_ || !binding_.valid()) {
            return false;
        }
        if (const auto* fragment_execution = std::get_if<
                bioseq::indexing::minimizer_fragment_generation_execution_receipt>(
                    &execution_)) {
            return index_kind_ == ready_minimizer_generation_index_kind::fragment
                && !minimizer_sequence_spill_resources_released_
                && minimizer_sequence_spill_generation_token_ == 0
                && minimizer_sequence_spill_mutation_before_release_ == 0
                && minimizer_sequence_spill_mutation_after_release_ == 0
                && fragment_execution->selected_backend == selected_backend_
                && fragment_execution->publication_accepted(binding_);
        }
        if (const auto* sequence_execution = std::get_if<
                bioseq::indexing::minimizer_sequence_generation_execution_receipt>(
                    &execution_)) {
            return index_kind_ == ready_minimizer_generation_index_kind::sequence
                && minimizer_sequence_spill_resources_released_
                && minimizer_sequence_spill_generation_token_ != 0
                && minimizer_sequence_spill_mutation_before_release_
                    != (std::numeric_limits<std::uint64_t>::max)()
                && minimizer_sequence_spill_mutation_after_release_
                    == minimizer_sequence_spill_mutation_before_release_ + 1u
                && sequence_execution->selected_backend == selected_backend_
                && sequence_execution->matches_publication(binding_)
                && sequence_execution->replay_final_storage_binding.generation_token
                    == minimizer_sequence_spill_generation_token_
                && sequence_execution->replay_final_storage_binding.mutation_revision
                    == minimizer_sequence_spill_mutation_before_release_;
        }
        return false;
    }
};

static_assert(!std::is_aggregate_v<ready_minimizer_generation_publication>);
static_assert(!std::is_default_constructible_v<
    ready_minimizer_generation_publication>);
static_assert(!std::is_copy_assignable_v<
    ready_minimizer_generation_publication>);
static_assert(!std::is_move_assignable_v<
    ready_minimizer_generation_publication>);
template <class p_publication>
concept publicly_relabelable_ready_minimizer_publication = requires(
        p_publication& publication,
        ready_minimizer_generation_execution_receipt execution) {
    publication.execution = execution;
    publication.generation_id = std::uint64_t{1};
    publication.source_identity = std::uint64_t{1};
    publication.selected_backend =
        bioseq::minimizers::minimizer_sketch_backend::scalar;
    publication.published = true;
};
static_assert(!publicly_relabelable_ready_minimizer_publication<
    ready_minimizer_generation_publication>);

/// Raise one coordinator diagnostic through a leaf PostgreSQL boundary. The
/// caller receives postgres_error only after PG_END_TRY restored the PG
/// exception stack, so no encoder, run-store, authorization, or writer object is
/// skipped by longjmp.
[[noreturn]] inline void ready_minimizer_error_as_cpp(
        int sqlstate,
        std::string_view message,
        std::string_view detail = {}) {
    bioseq::postgres::support::invoke_pg_as_cpp([sqlstate, message, detail] {
        if (detail.empty()) {
            ereport(ERROR,
                (errcode(sqlstate),
                 errmsg("%.*s", static_cast<int>(message.size()),
                    message.data())));
        }
        ereport(ERROR,
            (errcode(sqlstate),
             errmsg("%.*s", static_cast<int>(message.size()),
                message.data()),
             errdetail("%.*s", static_cast<int>(detail.size()),
                detail.data())));
    });
    pg_unreachable();
}

/// One bounded first-pass record.  The file is PostgreSQL-owned temporary
/// scratch and is never a publication artifact, so native words are sufficient:
/// it is rewound and consumed inside this statement by the same backend.
struct hash_major_minimizer_count_record {
    std::uint64_t hash = 0;
    std::uint64_t exact_count = 0;
};
static_assert(sizeof(hash_major_minimizer_count_record) == 16u);

/// PG BufFile scratch for hash-major's count-before-postings contract.  A first
/// replay writes one fixed record per distinct hash; the second replay reads
/// records in lockstep while postings stream directly into the logical encoder.
/// PostgreSQL's ResourceOwner owns a file after any ERROR.  Success closes it
/// explicitly; exception paths only abandon the C++ handle.
class hash_major_minimizer_count_scratch {
private:
    static constexpr std::size_t k_records_per_io = 4'096u;

    BufFile* file_ = nullptr;
    std::array<hash_major_minimizer_count_record, k_records_per_io> buffer_{};
    std::uint64_t record_count_ = 0u;
    std::uint64_t records_read_ = 0u;
    std::size_t buffered_records_ = 0u;
    std::size_t next_buffered_record_ = 0u;
    bool reading_ = false;

    void flush_write_buffer_() {
        if (buffered_records_ == 0u) return;
        const std::size_t bytes =
            buffered_records_ * sizeof(hash_major_minimizer_count_record);
        bioseq::postgres::support::invoke_pg_as_cpp([this, bytes] {
            BufFileWrite(file_, buffer_.data(), bytes);
        });
        buffered_records_ = 0u;
    }

public:
    hash_major_minimizer_count_scratch() {
        file_ = bioseq::postgres::support::invoke_pg_as_cpp(
            [] { return BufFileCreateTemp(false); });
        if (file_ == nullptr) {
            ready_minimizer_error_as_cpp(
                ERRCODE_OUT_OF_MEMORY,
                "hash-major hash-count scratch could not be created");
        }
    }

    hash_major_minimizer_count_scratch(
        const hash_major_minimizer_count_scratch&) = delete;
    hash_major_minimizer_count_scratch& operator=(
        const hash_major_minimizer_count_scratch&) = delete;
    hash_major_minimizer_count_scratch(
        hash_major_minimizer_count_scratch&&) = delete;
    hash_major_minimizer_count_scratch& operator=(
        hash_major_minimizer_count_scratch&&) = delete;
    ~hash_major_minimizer_count_scratch() = default;

    void append(const hash_major_minimizer_count_record& record) {
        if (file_ == nullptr || reading_ || records_read_ != 0u
            || record.exact_count == 0u
            || record_count_
                == (std::numeric_limits<std::uint64_t>::max)()) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "hash-major hash-count scratch append state is invalid");
        }
        buffer_[buffered_records_++] = record;
        if (buffered_records_ == buffer_.size()) {
            flush_write_buffer_();
        }
        ++record_count_;
    }

    void rewind_for_read() {
        if (file_ == nullptr || reading_ || records_read_ != 0u) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "hash-major hash-count scratch rewind state is invalid");
        }
        flush_write_buffer_();
        if (bioseq::postgres::support::invoke_pg_as_cpp([this] {
                    return BufFileSeek(file_, 0, 0, SEEK_SET);
                }) != 0) {
            ready_minimizer_error_as_cpp(
                ERRCODE_IO_ERROR,
                "hash-major hash-count scratch rewind failed");
        }
        reading_ = true;
        buffered_records_ = 0u;
        next_buffered_record_ = 0u;
    }

    [[nodiscard]] std::optional<hash_major_minimizer_count_record>
    read_next() {
        if (file_ == nullptr || !reading_
            || records_read_ > record_count_) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "hash-major hash-count scratch read state is invalid");
        }
        if (records_read_ == record_count_) {
            return std::nullopt;
        }
        if (next_buffered_record_ == buffered_records_) {
            const std::uint64_t remaining = record_count_ - records_read_;
            buffered_records_ = static_cast<std::size_t>(
                std::min<std::uint64_t>(remaining, buffer_.size()));
            next_buffered_record_ = 0u;
            const std::size_t bytes = buffered_records_
                * sizeof(hash_major_minimizer_count_record);
            bioseq::postgres::support::invoke_pg_as_cpp([this, bytes] {
                BufFileReadExact(file_, buffer_.data(), bytes);
            });
        }
        const hash_major_minimizer_count_record record =
            buffer_[next_buffered_record_++];
        ++records_read_;
        if (record.exact_count == 0u) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "hash-major hash-count scratch contains an empty hash");
        }
        return record;
    }

    [[nodiscard]] std::uint64_t record_count() const noexcept {
        return record_count_;
    }

    [[nodiscard]] std::uint64_t records_read() const noexcept {
        return records_read_;
    }

    void close_checked() {
        if (file_ != nullptr
            && (!reading_ || records_read_ != record_count_)) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "hash-major hash-count scratch closed before complete replay");
        }
        BufFile* closing = std::exchange(file_, nullptr);
        if (closing != nullptr) {
            bioseq::postgres::support::invoke_pg_as_cpp(
                [closing] { BufFileClose(closing); });
        }
    }

    void abandon_after_pg_error_noexcept() noexcept {
        file_ = nullptr;
    }
};

/// Validate and count one globally sorted replay without retaining postings.
/// The single target-wide segment used by the producer is what makes hash
/// ordering global instead of restarting at every persisted position segment.
struct hash_major_minimizer_count_replay {
    hash_major_minimizer_count_scratch* scratch = nullptr;
    std::uint64_t target_length = 0u;
    std::uint64_t maximum_hash = 0u;
    std::uint64_t current_hash = 0u;
    std::uint64_t current_count = 0u;
    std::uint64_t previous_position = 0u;
    std::uint64_t event_count = 0u;
    bool have_hash = false;

    [[nodiscard]] std::expected<
        void, bioseq::indexing::minimizer_generation_encoding_error>
    flush_hash() {
        if (!have_hash) return {};
        if (scratch == nullptr || current_count == 0u) {
            return std::unexpected(
                bioseq::indexing::minimizer_generation_encoding_error::row_count);
        }
        scratch->append(hash_major_minimizer_count_record{
            .hash = current_hash,
            .exact_count = current_count,
        });
        current_count = 0u;
        return {};
    }

    [[nodiscard]] std::expected<
        void, bioseq::indexing::minimizer_generation_encoding_error>
    accept(const bioseq::indexing::canonical_minimizer_event& event) {
        const std::uint64_t position = event.encoded_position >> 1u;
        if (scratch == nullptr || target_length == 0u
            || event.hash > maximum_hash || position >= target_length) {
            return std::unexpected(
                event.hash > maximum_hash
                    ? bioseq::indexing::minimizer_generation_encoding_error::hash_range
                    : bioseq::indexing::minimizer_generation_encoding_error::
                        position_range);
        }
        if (have_hash) {
            if (event.hash < current_hash
                || (event.hash == current_hash
                    && event.encoded_position <= previous_position)) {
                return std::unexpected(
                    event.hash == current_hash
                        && event.encoded_position == previous_position
                    ? bioseq::indexing::minimizer_generation_encoding_error::
                        duplicate_event
                    : bioseq::indexing::minimizer_generation_encoding_error::
                        row_order);
            }
            if (event.hash != current_hash) {
                if (auto flushed = flush_hash(); !flushed) return flushed;
                current_hash = event.hash;
            }
        } else {
            current_hash = event.hash;
            have_hash = true;
        }
        if (current_count
                == (std::numeric_limits<std::uint64_t>::max)()
            || event_count
                == (std::numeric_limits<std::uint64_t>::max)()) {
            return std::unexpected(
                bioseq::indexing::minimizer_generation_encoding_error::
                    arithmetic_overflow);
        }
        ++current_count;
        ++event_count;
        previous_position = event.encoded_position;
        return {};
    }

    [[nodiscard]] std::expected<
        void, bioseq::indexing::minimizer_generation_encoding_error>
    finish() {
        return flush_hash();
    }
};

/// Consume the second identical replay in lockstep with first-pass count
/// records.  Only the current 16-byte record and encoder-owned row buffers are
/// live, even when one hash owns billions of postings.
struct hash_major_minimizer_encode_replay {
    hash_major_minimizer_count_scratch* scratch = nullptr;
    bioseq::indexing::hash_major_minimizer_logical_encoder* encoder = nullptr;
    std::optional<bioseq::indexing::hash_major_minimizer_error>* encoding_error =
        nullptr;
    std::uint64_t target_length = 0u;
    std::uint64_t maximum_hash = 0u;
    std::uint64_t current_hash = 0u;
    std::uint64_t previous_position = 0u;
    std::uint64_t event_count = 0u;
    bool hash_open = false;

    [[nodiscard]] std::expected<
        void, bioseq::indexing::minimizer_generation_encoding_error>
    report_encoding_error(bioseq::indexing::hash_major_minimizer_error error) {
        if (encoding_error != nullptr) {
            *encoding_error = error;
        }
        return std::unexpected(
            bioseq::indexing::minimizer_generation_encoding_error::
                storage_contract_mismatch);
    }

    [[nodiscard]] std::expected<
        void, bioseq::indexing::minimizer_generation_encoding_error>
    close_hash() {
        if (!hash_open) return {};
        if (auto ended = encoder->end_hash(); !ended) {
            return report_encoding_error(ended.error());
        }
        hash_open = false;
        return {};
    }

    [[nodiscard]] std::expected<
        void, bioseq::indexing::minimizer_generation_encoding_error>
    open_hash(std::uint64_t hash) {
        const auto record = scratch->read_next();
        if (!record || record->hash != hash) {
            return std::unexpected(
                bioseq::indexing::minimizer_generation_encoding_error::
                    replay_mismatch);
        }
        if (auto begun = encoder->begin_hash(
                record->hash, record->exact_count); !begun) {
            return report_encoding_error(begun.error());
        }
        current_hash = hash;
        hash_open = true;
        return {};
    }

    [[nodiscard]] std::expected<
        void, bioseq::indexing::minimizer_generation_encoding_error>
    accept(const bioseq::indexing::canonical_minimizer_event& event) {
        const std::uint64_t position = event.encoded_position >> 1u;
        if (scratch == nullptr || encoder == nullptr || target_length == 0u
            || event.hash > maximum_hash || position >= target_length) {
            return std::unexpected(
                bioseq::indexing::minimizer_generation_encoding_error::
                    replay_mismatch);
        }
        if (!hash_open || event.hash != current_hash) {
            if (hash_open && event.hash < current_hash) {
                return std::unexpected(
                    bioseq::indexing::minimizer_generation_encoding_error::
                        replay_mismatch);
            }
            if (auto closed = close_hash(); !closed) return closed;
            if (auto opened = open_hash(event.hash); !opened) return opened;
        } else if (event.encoded_position <= previous_position) {
            return std::unexpected(
                bioseq::indexing::minimizer_generation_encoding_error::
                    replay_mismatch);
        }
        if (auto appended = encoder->append_posting(
                event.encoded_position); !appended) {
            return report_encoding_error(appended.error());
        }
        if (event_count
                == (std::numeric_limits<std::uint64_t>::max)()) {
            return std::unexpected(
                bioseq::indexing::minimizer_generation_encoding_error::
                    arithmetic_overflow);
        }
        ++event_count;
        previous_position = event.encoded_position;
        return {};
    }

    [[nodiscard]] std::expected<
        void, bioseq::indexing::minimizer_generation_encoding_error>
    finish() {
        if (auto closed = close_hash(); !closed) return closed;
        if (scratch == nullptr
            || scratch->records_read() != scratch->record_count()) {
            return std::unexpected(
                bioseq::indexing::minimizer_generation_encoding_error::
                    replay_mismatch);
        }
        return {};
    }
};

/// Minimal validated result for the production hash-major publication. The
/// private storage token has already enforced complete payload/manifest reread
/// before this value can exist, and the published head is byte-compared below.
struct ready_hash_major_minimizer_publication {
    std::uint64_t generation_id = 0u;
    std::uint64_t query_key = 0u;
    std::uint64_t source_epoch = 0u;
    std::uint64_t source_content_crc64 = 0u;
    std::uint64_t unique_hash_count = 0u;
    std::uint64_t total_posting_count = 0u;
    bioseq::minimizers::minimizer_sketch_backend selected_backend =
        bioseq::minimizers::minimizer_sketch_backend::scalar;
    bioseq::indexing::hash_major_minimizer_head head{};
    bioseq::indexing::hash_major_minimizer_manifest manifest{};
    std::optional<std::uint64_t> replaced_generation_id{};
    bool published = false;
};

/// Build and atomically publish one sequence-wide hash-major minimizer generation
/// from a fragmented PostgreSQL source. Native minimizers are collected into
/// PostgreSQL-owned sorted spill runs; two bounded replays then satisfy the
/// encoder's exact-count-before-postings contract. Fragment indexes are
/// intentionally excluded: their physical indexes must never cross this merge seam.
template <class p_sequence_range>
[[nodiscard]] inline ready_hash_major_minimizer_publication
build_and_publish_hash_major_minimizer_generation_pg(
        Oid user_relid,
        std::int16_t column_attno,
        std::string_view index_name,
        std::uint64_t sequence_rowno,
        fragment_index_storage& storage,
        bioseq::storage::lazy_sequence_view<
            typename bioseq::storage::postgres_lazy_sequence_binding<p_sequence_range>::
                alphabet_type> sequence,
        bioseq::storage::postgres_lazy_sequence_binding<p_sequence_range> source_binding,
        bioseq::minimizers::minimizer_sketch_backend backend,
        const bioseq::minimizers::minimizer_kernels* kernels) {
    using source_binding_type =
        bioseq::storage::postgres_lazy_sequence_binding<p_sequence_range>;
    using backend_type =
        bioseq::minimizers::minimizer_sketch_backend;
    using sequence_index =
        bioseq::indexing::direct_minimizer_bucket_index<fragment_index_storage>;
    require_selected_minimizer_kernels(
        backend, kernels, "hash-major minimizer generation");
    using traits_type = typename compact_sequence_range<p_sequence_range>::traits;
    using canonical_sequence_config = sequence_custom_alphabet<
        typename traits_type::alphabet_type,
        typename traits_type::storage_unit>;
    static_assert(traits_type::bits_per_char
        == compact_sequence_range<canonical_sequence_config>::traits::
            bits_per_char,
        "hash-major publisher requires canonical PostgreSQL packing");
    const sequence_locator& locator = source_binding.locator();
    Snapshot registered_source_snapshot =
        source_binding.registered_snapshot();

    const bool is_sequence_index =
        index_name == k_minimizers_per_sequence_index_name;
    const bool active_snapshot_ready =
        bioseq::postgres::support::invoke_pg_as_cpp([] {
            return ActiveSnapshotSet()
                && IsMVCCSnapshot(GetActiveSnapshot());
        });
    if (!OidIsValid(user_relid) || column_attno <= 0 || !is_sequence_index
        || source_binding.column_attno() != column_attno
        || !bioseq::indexing::sql_bigint_can_represent(sequence_rowno)
        || locator.payload_version != sequence_locator_payload_version || locator.source_epoch == 0u
        || locator.logical_length == 0u || locator.fragment_count == 0u
        || registered_source_snapshot == nullptr
        || !IsMVCCSnapshot(registered_source_snapshot)
        || registered_source_snapshot->regd_count == 0
        || !active_snapshot_ready) {
        ready_minimizer_error_as_cpp(
            ERRCODE_INVALID_PARAMETER_VALUE,
            "hash-major minimizer generation coordinator input is invalid");
    }
    if (backend != backend_type::scalar && backend != backend_type::exact_simd
        && backend != backend_type::bounded_nthash_simd
        && backend != backend_type::vulkan_segment) {
        ready_minimizer_error_as_cpp(
            ERRCODE_INVALID_PARAMETER_VALUE,
            "hash-major minimizer generation backend is invalid");
    }
    constexpr std::uint16_t k_window_size = 10u;
    constexpr std::uint16_t k_kmer_size = 15u;
    if (static_cast<std::uint64_t>(sequence.bp_size())
            != locator.logical_length) {
        ready_minimizer_error_as_cpp(
            ERRCODE_DATA_CORRUPTED,
            "hash-major minimizer lazy sequence shape disagrees with its locator");
    }

    const relation_identity relation_identity =
        bioseq::postgres::support::invoke_pg_as_cpp(
            [user_relid] { return index_relation_identity(user_relid); });
    if (relation_identity.db_oid != MyDatabaseId
        || !OidIsValid(relation_identity.tablespace_oid)
        || !OidIsValid(relation_identity.relfilenode)
        || storage.db_oid_ != relation_identity.db_oid
        || storage.tablespace_oid_ != relation_identity.tablespace_oid
        || storage.main_relfilenode_ != relation_identity.relfilenode
        || storage.user_relid_ != user_relid
        || storage.column_attno_ != column_attno
        || storage.index_name_ != index_name
        || storage.bucket_idx_ != locator.bucket_index
        || storage.shard_idx_ != locator.shard_index
        || storage.query_key_ != sequence_rowno
        || source_binding.db_oid() != relation_identity.db_oid
        || source_binding.tablespace_oid()
            != relation_identity.tablespace_oid
        || source_binding.main_relfilenode()
            != relation_identity.relfilenode) {
        ready_minimizer_error_as_cpp(
            ERRCODE_DATA_CORRUPTED,
            "hash-major minimizer storage identity disagrees with its source");
    }

    const bioseq::indexing::minimizer_scheme_identity scheme =
        bioseq::indexing::minimizer_scheme_identity_for_backend(backend);

    struct build_state {
        minimizer_generation_administration_identity identity{};
        std::optional<minimizer_generation_source_authorization> source_authorization;
        minimizer_generation_source_locator source{};
        std::uint64_t source_content_checksum = 0u;
        std::optional<index_storage_hash_major_minimizer_writer> writer;
        std::optional<hash_major_minimizer_count_scratch> count_scratch;
        std::optional<ready_hash_major_minimizer_token> ready_token;
        std::optional<bioseq::indexing::hash_major_minimizer_error> encoding_error;
        bioseq::indexing::minimizer_sequence_generation_execution_receipt execution{};
    };
    auto* state = new build_state{
        .identity = minimizer_generation_administration_identity{
            .database_oid = relation_identity.db_oid,
            .tablespace_oid = relation_identity.tablespace_oid,
            .main_relfilenode = relation_identity.relfilenode,
            .user_relid = user_relid,
            .column_attno = column_attno,
            .index_name = std::string{index_name},
            .table_name = storage.this_table_name(),
        },
    };
    ready_hash_major_minimizer_publication result{
        .query_key = sequence_rowno,
        .source_epoch = locator.source_epoch,
        .selected_backend = backend,
    };

    try {
        scoped_forced_bounded_nthash_index_failure forced_bounded_failure;
        state->source_authorization.emplace(
            authorize_minimizer_generation_source_pg(
                state->identity, sequence_rowno,
                registered_source_snapshot));
        state->source =
            state->source_authorization->source_for_diagnostics();
        state->source_content_checksum = state->source_authorization
            ->source_content_checksum_for_diagnostics();
        if (state->source.sequence_rowno != sequence_rowno
            || !same_minimizer_generation_sequence_locator(state->source.locator, locator)
            || state->source.alphabet_code
                != source_binding_type::alphabet_code()
            || state->source.storage_unit_byte_width
                != source_binding_type::storage_unit_byte_width()
            || state->source.padding_letters
                != source_binding.last_fragment_padding()
            || state->source.locator.logical_length != sequence.bp_size()
            || state->source.locator.fragment_count != locator.fragment_count) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "hash-major authoritative source authorization disagrees "
                "with the passed lazy sequence");
        }
        result.source_content_crc64 = state->source_content_checksum;

        const auto derived_profile = bioseq::indexing::
            hash_major_minimizer_profile_for_backend(
                backend,
                state->source.alphabet_code,
                k_kmer_size,
                k_window_size,
                locator.logical_length);
        if (!derived_profile) {
            ready_minimizer_error_as_cpp(
                ERRCODE_FEATURE_NOT_SUPPORTED,
                "source/backend combination has no canonical hash-major profile",
                fmt::format(
                    "backend={} alphabet_code={} k={} w={} encoding_error={}",
                    bioseq::minimizers::
                        minimizer_sketch_backend_name(backend),
                    state->source.alphabet_code,
                    k_kmer_size, k_window_size,
                    static_cast<unsigned>(derived_profile.error())));
        }
        const bioseq::indexing::hash_major_minimizer_storage_profile profile =
            *derived_profile;

        ensure_vulkan_index_build_runtime_ready(backend);
        result.generation_id = bioseq::postgres::support::invoke_pg_as_cpp(
            [] { return allocate_minimizer_generation_id_pg(); });
        state->writer.emplace(storage.begin_hash_major_minimizer_generation_write(
            sequence_rowno, result.generation_id));
        state->count_scratch.emplace();
        forced_bounded_failure.arm_after_publisher_state_begin(backend);

        const bioseq::indexing::hash_major_minimizer_generation_options options{
            .generation_id = result.generation_id,
            .query_key = sequence_rowno,
            .source_epoch = locator.source_epoch,
            .source_content_crc64 = state->source_content_checksum,
            .target_length = locator.logical_length,
            .storage_profile = profile,
        };
        const std::uint64_t maximum_hash =
            bioseq::indexing::hash_major_minimizer_max_hash(profile);

        auto spill = sequence_index::spill_generation_replay_runs_serial(
            storage,
            sequence,
            backend,
            kernels,
            /*core_span_bp=*/0u,
            /*position_segment_span_bp=*/locator.logical_length,
            &state->execution,
            state->source.alphabet_code);
        if (!spill) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "hash-major minimizer spill producer failed",
                fmt::format(
                    "backend={} wire_error={}",
                    bioseq::minimizers::
                        minimizer_sketch_backend_name(backend),
                    static_cast<unsigned>(spill.error())));
        }

        std::optional<bioseq::indexing::hash_major_minimizer_encoded_metadata>
            encoded_metadata;
        {
            auto producer = sequence_index::make_generation_replay_producer(
                storage,
                locator.logical_length,
                std::move(*spill),
                locator.logical_length);

            hash_major_minimizer_count_replay count_pass{
                .scratch = &*state->count_scratch,
                .target_length = locator.logical_length,
                .maximum_hash = maximum_hash,
            };
            auto counted = producer.scan_sorted_events(
                locator.logical_length,
                bioseq::indexing::canonical_minimizer_event_receiver{
                    .context = &count_pass,
                    .emit_fn = +[](
                            void* context,
                            const bioseq::indexing::canonical_minimizer_event&
                                event)
                        -> std::expected<
                            void,
                            bioseq::indexing::minimizer_generation_encoding_error> {
                        return static_cast<hash_major_minimizer_count_replay*>(
                            context)->accept(event);
                    },
                });
            if (counted) counted = count_pass.finish();
            if (!counted) {
                ready_minimizer_error_as_cpp(
                    ERRCODE_DATA_CORRUPTED,
                    "hash-major count replay failed",
                    fmt::format(
                        "wire_error={}",
                        static_cast<unsigned>(counted.error())));
            }
            state->count_scratch->rewind_for_read();

            bioseq::indexing::hash_major_minimizer_logical_encoder encoder{
                options,
                state->writer->logical_stream_sink(),
            };
            hash_major_minimizer_encode_replay encode_pass{
                .scratch = &*state->count_scratch,
                .encoder = &encoder,
                .encoding_error = &state->encoding_error,
                .target_length = locator.logical_length,
                .maximum_hash = maximum_hash,
            };
            auto emitted = producer.scan_sorted_events(
                locator.logical_length,
                bioseq::indexing::canonical_minimizer_event_receiver{
                    .context = &encode_pass,
                    .emit_fn = +[](
                            void* context,
                            const bioseq::indexing::canonical_minimizer_event&
                                event)
                        -> std::expected<
                            void,
                            bioseq::indexing::minimizer_generation_encoding_error> {
                        return static_cast<hash_major_minimizer_encode_replay*>(
                            context)->accept(event);
                    },
                });
            if (emitted) emitted = encode_pass.finish();
            if (!emitted
                || count_pass.event_count != encode_pass.event_count) {
                ready_minimizer_error_as_cpp(
                    ERRCODE_DATA_CORRUPTED,
                    "hash-major posting replay failed",
                    fmt::format(
                        "wire_error={} encoding_error={} counted={} emitted={}",
                        emitted
                            ? 0u
                            : static_cast<unsigned>(emitted.error()) + 1u,
                        state->encoding_error
                            ? static_cast<unsigned>(*state->encoding_error) + 1u
                            : 0u,
                        count_pass.event_count,
                        encode_pass.event_count));
            }
            auto finished = encoder.finish();
            if (!finished) {
                state->encoding_error = finished.error();
                ready_minimizer_error_as_cpp(
                    ERRCODE_DATA_CORRUPTED,
                    "hash-major logical encoder failed",
                    fmt::format(
                        "encoding_error={}",
                        static_cast<unsigned>(finished.error())));
            }
            encoded_metadata.emplace(std::move(*finished));
            state->execution = producer.execution_receipt();
        }

        const auto expected_source_identity =
            bioseq::indexing::hash_major_minimizer_source_identity(
                sequence_rowno,
                locator.source_epoch,
                state->source_content_checksum,
                locator.logical_length);
        const auto expected_storage_profile_digest =
            bioseq::indexing::hash_major_minimizer_storage_profile_digest(
                profile);
        const auto& metadata = *encoded_metadata;
        if (metadata.head.generation_id != result.generation_id
            || metadata.head.query_key != sequence_rowno
            || metadata.head.source_epoch != locator.source_epoch
            || metadata.head.source_identity != expected_source_identity
            || metadata.head.source_content_crc64
                != state->source_content_checksum
            || metadata.head.storage_profile_digest
                != expected_storage_profile_digest
            || metadata.head.dense_seal.has_value()
            || metadata.manifest.generation_id != result.generation_id
            || metadata.manifest.source_epoch != locator.source_epoch
            || metadata.manifest.source_identity != expected_source_identity
            || metadata.manifest.source_content_crc64
                != state->source_content_checksum
            || metadata.manifest.target_length != locator.logical_length
            || metadata.manifest.storage_profile != profile
            || metadata.manifest.storage_profile_digest
                != expected_storage_profile_digest
            || metadata.manifest.unique_hash_count
                != state->count_scratch->record_count()
            || metadata.manifest.total_posting_count
                != state->execution.spilled_event_count) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "hash-major encoded metadata identity disagrees");
        }

        const auto before_release = storage.segment_spill_storage_binding();
        const std::uint64_t expected_visited =
            state->execution.spilled_event_count
                    <= (std::numeric_limits<std::uint64_t>::max)() / 2u
                ? state->execution.spilled_event_count * 2u
                : (std::numeric_limits<std::uint64_t>::max)();
        if (!state->execution.is_spill_valid()
            || state->execution.selected_backend != backend
            || state->execution.minimizer_scheme != scheme
            || state->execution.target_length != locator.logical_length
            || state->execution.window_size != k_window_size
            || state->execution.kmer_size != k_kmer_size
            || state->execution.generation_layout
                != bioseq::indexing::minimizer_generation_layout::invalid
            || state->execution.replay_scan_count != 2u
            || state->execution.replay_merge_pass_count != 2u
            || state->execution.replay_reroute_pass_count != 0u
            || state->execution.replay_completed_scan_count != 2u
            || state->execution.replay_failed_scan_count != 0u
            || state->execution.replay_binding_checked_failed_scan_count != 0u
            || state->execution.replay_illegal_span_transition_count != 0u
            || state->execution.replay_first_completed_event_count
                != state->execution.spilled_event_count
            || state->execution.replay_last_completed_event_count
                != state->execution.spilled_event_count
            || state->execution.replay_first_completed_content_digest
                != state->execution.spill_content_digest
            || state->execution.replay_last_completed_content_digest
                != state->execution.spill_content_digest
            || state->execution.replay_final_span_bp
                != locator.logical_length
            || state->execution.replay_final_storage_binding
                != before_release
            || state->execution.replay_visited_event_count != expected_visited
            || !before_release.valid()) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "hash-major selected backend replay validation was rejected");
        }

        storage.release_runs();
        const auto after_release = storage.segment_spill_storage_binding();
        if (after_release.generation_token != before_release.generation_token
            || before_release.mutation_revision
                == (std::numeric_limits<std::uint64_t>::max)()
            || after_release.mutation_revision
                != before_release.mutation_revision + 1u) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "hash-major spill resources did not release atomically");
        }
        state->count_scratch->close_checked();
        state->count_scratch.reset();

        result.unique_hash_count = metadata.manifest.unique_hash_count;
        result.total_posting_count =
            metadata.manifest.total_posting_count;
        result.head = metadata.head;
        result.manifest = metadata.manifest;
        state->ready_token.emplace(
            state->writer->seal_and_validate_ready(
                metadata, std::move(*state->source_authorization)));
        state->source_authorization.reset();
        state->writer.reset();
        storage.close_checked();
        inject_minimizer_generation_failure_as_cpp(
            minimizer_generation_failure_injection::generation_storage_close,
            "hash_major_minimizer_generation_storage_close");

        ready_hash_major_minimizer_published_head_receipt publication_receipt =
            publish_ready_hash_major_minimizer_head_pg(
                std::move(*state->ready_token));
        state->ready_token.reset();
        if (publication_receipt.query_key != sequence_rowno
            || publication_receipt.head != metadata.head
            || publication_receipt.head_bytes != metadata.encoded_head) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "hash-major publication did not return the validated byte-exact head");
        }
        result.replaced_generation_id =
            publication_receipt.replaced_generation_id;
        result.published = true;
        delete state;
        state = nullptr;
    } catch (const bioseq::postgres::support::postgres_error&) {
        storage.abandon_after_pg_error_noexcept();
        if (state->count_scratch) {
            state->count_scratch->abandon_after_pg_error_noexcept();
        }
        state->ready_token.reset();
        state->writer.reset();
        state->source_authorization.reset();
        delete state;
        state = nullptr;
        throw;
    } catch (...) {
        storage.abandon_after_pg_error_noexcept();
        if (state->count_scratch) {
            state->count_scratch->abandon_after_pg_error_noexcept();
        }
        state->ready_token.reset();
        state->writer.reset();
        state->source_authorization.reset();
        delete state;
        state = nullptr;
        throw;
    }
    return result;
}

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
            typename bioseq::storage::postgres_lazy_sequence_binding<p_sequence_range>::
                alphabet_type> sequence,
        bioseq::storage::postgres_lazy_sequence_binding<p_sequence_range> source_binding,
        bioseq::minimizers::minimizer_sketch_backend backend,
        const bioseq::minimizers::minimizer_kernels* kernels) {
    using source_binding_type =
        bioseq::storage::postgres_lazy_sequence_binding<p_sequence_range>;
    const sequence_locator& locator = source_binding.locator();
    constexpr std::uint16_t k_window_size = 10u;
    constexpr std::uint16_t k_kmer_size = 15u;
    constexpr std::uint16_t k_alphabet_code =
        source_binding_type::alphabet_code();
    if (index_name == k_minimizers_per_fragment_index_name) {
        // The fragment route owns one persisted index per physical source fragment. Never
        // route it through the target-wide hash-major spill/merge publisher.
        build_and_verify_grouped_soa_minimizer_generation_pg(
            user_relid, column_attno, index_name, sequence_rowno, storage,
            sequence, source_binding, backend, kernels);
        return;
    }
    if constexpr (k_alphabet_code != 0u) {
        // Avoid instantiating the large hash-major publisher for alphabet
        // specializations which are statically outside its registry.
        build_and_verify_grouped_soa_minimizer_generation_pg(
            user_relid, column_attno, index_name, sequence_rowno, storage,
            sequence, source_binding, backend, kernels);
        return;
    } else {
        const auto profile = bioseq::indexing::
            hash_major_minimizer_profile_for_backend(
                backend,
                k_alphabet_code,
                k_kmer_size,
                k_window_size,
                locator.logical_length);
        if (!profile) {
            // Future DNA4 parameter profiles which are not in the hash-major
            // registry continue on the grouped-SoA route.
            build_and_verify_grouped_soa_minimizer_generation_pg(
                user_relid, column_attno, index_name, sequence_rowno,
                storage, sequence, source_binding, backend, kernels);
            return;
        }
        const ready_hash_major_minimizer_publication publication =
            build_and_publish_hash_major_minimizer_generation_pg(
                user_relid, column_attno, index_name, sequence_rowno,
                storage, sequence, source_binding, backend, kernels);
        if (!publication.published
            || publication.generation_id == 0u
            || publication.query_key != sequence_rowno
            || publication.source_epoch != locator.source_epoch
            || publication.selected_backend != backend
            || publication.head.generation_id != publication.generation_id
            || publication.head.query_key != sequence_rowno
            || publication.head.source_epoch != locator.source_epoch
            || publication.manifest.generation_id != publication.generation_id
            || publication.manifest.source_epoch != locator.source_epoch
            || publication.manifest.target_length != locator.logical_length
            || publication.unique_hash_count
                != publication.manifest.unique_hash_count
            || publication.total_posting_count
                != publication.manifest.total_posting_count) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "normal hash-major minimizer rebuild publication validation was rejected",
                fmt::format(
                    "index={} backend={}",
                    index_name,
                    bioseq::minimizers::
                        minimizer_sketch_backend_name(backend)));
        }
    }
}

/// Build, completely reread, and atomically publish one grouped-SoA fragment
/// or sequence minimizer generation from an authoritative PostgreSQL-backed
/// lazy sequence.
///
/// Ordinary update_index uses this route for PostgreSQL-backed
/// alphabet/parameter profiles not registered by hash-major. Grouped-SoA fixtures
/// and readers use this route; DNA4/15/10 production publication uses the
/// hash-major coordinator above.
/// The caller retains both the registered source snapshot and metadata-age
/// ownership. `storage.begin_generation_write()` creates a missing canonical
/// sidecar or reuses an exact one; noncanonical same-name relations require
/// an explicit lifecycle clear before this route. Publication remains
/// impossible until the sink has returned its private complete-reread token.
template <class p_sequence_range>
[[nodiscard]] inline ready_minimizer_generation_publication
build_and_publish_grouped_soa_minimizer_generation_pg(
        Oid user_relid,
        std::int16_t column_attno,
        std::string_view index_name,
        std::uint64_t sequence_rowno,
        fragment_index_storage& storage,
        bioseq::storage::lazy_sequence_view<
            typename bioseq::storage::postgres_lazy_sequence_binding<p_sequence_range>::
                alphabet_type> sequence,
        bioseq::storage::postgres_lazy_sequence_binding<p_sequence_range> source_binding,
        bioseq::minimizers::minimizer_sketch_backend backend,
        const bioseq::minimizers::minimizer_kernels* kernels) {
    using source_binding_type =
        bioseq::storage::postgres_lazy_sequence_binding<p_sequence_range>;
    using backend_type =
        bioseq::minimizers::minimizer_sketch_backend;
    using fragment_index =
        bioseq::indexing::minimizers_per_fragment_index<fragment_index_storage>;
    using sequence_index =
        bioseq::indexing::direct_minimizer_bucket_index<fragment_index_storage>;
    require_selected_minimizer_kernels(
        backend, kernels, "grouped-SoA minimizer generation");
    using traits_type = typename compact_sequence_range<p_sequence_range>::traits;
    using canonical_sequence_config = sequence_custom_alphabet<
        typename traits_type::alphabet_type,
        typename traits_type::storage_unit>;
    static_assert(traits_type::bits_per_char
        == compact_sequence_range<canonical_sequence_config>::traits::
            bits_per_char,
        "READY minimizer coordinator requires canonical PostgreSQL packing");
    const sequence_locator& locator = source_binding.locator();
    Snapshot registered_source_snapshot =
        source_binding.registered_snapshot();

    const bool is_fragment_index =
        index_name == k_minimizers_per_fragment_index_name;
    const bool is_sequence_index =
        index_name == k_minimizers_per_sequence_index_name;
    const bool active_snapshot_ready =
        bioseq::postgres::support::invoke_pg_as_cpp([] {
            return ActiveSnapshotSet()
                && IsMVCCSnapshot(GetActiveSnapshot());
        });

    if (!OidIsValid(user_relid) || column_attno <= 0
        || source_binding.column_attno() != column_attno
        || (!is_fragment_index && !is_sequence_index)
        || !bioseq::indexing::sql_bigint_can_represent(sequence_rowno)
        || locator.payload_version != sequence_locator_payload_version || locator.source_epoch == 0
        || locator.logical_length == 0 || locator.fragment_count == 0
        || registered_source_snapshot == nullptr
        || !IsMVCCSnapshot(registered_source_snapshot)
        || registered_source_snapshot->regd_count == 0
        || !active_snapshot_ready) {
        ready_minimizer_error_as_cpp(
            ERRCODE_INVALID_PARAMETER_VALUE,
            "READY minimizer generation coordinator input is invalid");
    }
    if (backend != backend_type::scalar && backend != backend_type::exact_simd
        && backend != backend_type::bounded_nthash_simd
        && backend != backend_type::vulkan_segment) {
        ready_minimizer_error_as_cpp(
            ERRCODE_INVALID_PARAMETER_VALUE,
            "READY minimizer generation backend is invalid");
    }
    constexpr std::uint16_t k_window_size = 10u;
    constexpr std::uint16_t k_kmer_size = 15u;
    constexpr std::uint64_t k_max_occ_cap =
        bioseq::indexing::k_grouped_soa_default_max_occurrence_cap;
    constexpr std::uint16_t k_alphabet_code =
        source_binding_type::alphabet_code();
    constexpr std::uint64_t k_fragment_span_bp =
        source_binding_type::fragment_bp_capacity();
    static_assert(k_fragment_span_bp != 0);
    const std::uint64_t persisted_segment_span_bp = is_fragment_index
        ? k_fragment_span_bp
        : bioseq::indexing::k_minimizer_preferred_segment_span_bp;
    if (static_cast<std::uint64_t>(sequence.bp_size())
            != locator.logical_length) {
        ready_minimizer_error_as_cpp(
            ERRCODE_DATA_CORRUPTED,
            "READY minimizer lazy sequence shape disagrees with its locator");
    }

    const relation_identity relation_identity =
        bioseq::postgres::support::invoke_pg_as_cpp(
            [user_relid] { return index_relation_identity(user_relid); });
    if (relation_identity.db_oid != MyDatabaseId
        || !OidIsValid(relation_identity.tablespace_oid)
        || !OidIsValid(relation_identity.relfilenode)
        || storage.db_oid_ != relation_identity.db_oid
        || storage.tablespace_oid_ != relation_identity.tablespace_oid
        || storage.main_relfilenode_ != relation_identity.relfilenode
        || storage.user_relid_ != user_relid
        || storage.column_attno_ != column_attno
        || storage.index_name_ != index_name
        || storage.bucket_idx_ != locator.bucket_index
        || storage.shard_idx_ != locator.shard_index
        || storage.query_key_ != sequence_rowno
        || source_binding.db_oid() != relation_identity.db_oid
        || source_binding.tablespace_oid()
            != relation_identity.tablespace_oid
        || source_binding.main_relfilenode()
            != relation_identity.relfilenode) {
        ready_minimizer_error_as_cpp(
            ERRCODE_DATA_CORRUPTED,
            "READY minimizer storage identity disagrees with its source");
    }

    if constexpr (k_alphabet_code
            != bioseq::indexing::k_minimizer_persisted_dna4_alphabet_code) {
        if (backend == backend_type::bounded_nthash_simd) {
            // Preserve the selected producer's typed capability reason before
            // the grouped-SoA registry reports the broader profile mismatch.
            throw bioseq::indexing::packed_bounded_nthash_index_runtime_error{
                bioseq::indexing::packed_bounded_nthash_index_status::
                    unsupported_alphabet,
                "grouped minimizer generation preflight"};
        }
    }

    const bioseq::indexing::minimizer_scheme_identity scheme =
        bioseq::indexing::minimizer_scheme_identity_for_backend(backend);
    const bioseq::indexing::minimizer_producer_identity producer_backend =
        bioseq::indexing::minimizer_producer_backend_identity_for_backend(
            backend);
    bioseq::indexing::grouped_soa_minimizer_profile profile{};
    {
        const auto derived_profile =
            bioseq::indexing::derive_grouped_soa_minimizer_profile(
                scheme, k_alphabet_code, k_kmer_size,
                persisted_segment_span_bp,
                k_max_occ_cap);
        if (!derived_profile) {
            ready_minimizer_error_as_cpp(
                ERRCODE_FEATURE_NOT_SUPPORTED,
                "READY minimizer backend has no canonical grouped-SoA profile",
                fmt::format("backend={} wire_error={}",
                    bioseq::minimizers::
                        minimizer_sketch_backend_name(backend),
                    static_cast<unsigned>(derived_profile.error())));
        }
        profile = *derived_profile;
    }

    struct build_state {
        minimizer_generation_administration_identity identity{};
        std::optional<minimizer_generation_source_authorization> source_authorization;
        minimizer_generation_source_locator source{};
        std::uint64_t source_content_checksum = 0;
        std::optional<index_storage_fragment_generation_row_sink> writer;
        std::optional<bioseq::indexing::encoded_grouped_soa_generation>
            generation;
        std::optional<ready_minimizer_generation_token> ready_token;
        std::optional<bioseq::indexing::minimizer_generation_encoding_error>
            encode_error;
        bioseq::indexing::minimizer_fragment_generation_execution_receipt
            fragment_execution{};
        bioseq::indexing::minimizer_sequence_generation_execution_receipt
            sequence_execution{};
        ready_minimizer_generation_execution_receipt execution{};
        bool sequence_spill_started = false;
    };
    auto* state = new build_state{
        .identity = minimizer_generation_administration_identity{
            .database_oid = relation_identity.db_oid,
            .tablespace_oid = relation_identity.tablespace_oid,
            .main_relfilenode = relation_identity.relfilenode,
            .user_relid = user_relid,
            .column_attno = column_attno,
            .index_name = std::string{index_name},
            .table_name = storage.this_table_name(),
        },
    };
    struct publication_draft {
        ready_minimizer_generation_index_kind index_kind =
            ready_minimizer_generation_index_kind::fragment;
        bioseq::indexing::minimizer_generation_publication_binding binding{};
        std::uint64_t generation_id = 0;
        std::uint64_t source_identity = 0;
        std::uint64_t source_content_checksum = 0;
        std::uint64_t storage_contract_checksum = 0;
        std::uint64_t control_count = 0;
        std::uint64_t position_count = 0;
        std::uint64_t writer_rows = 0;
        std::uint64_t writer_batches = 0;
        bioseq::minimizers::minimizer_sketch_backend selected_backend =
            bioseq::minimizers::minimizer_sketch_backend::scalar;
        ready_minimizer_generation_execution_receipt execution{};
        std::optional<std::uint64_t> replaced_generation_id{};
        std::uint64_t minimizer_sequence_spill_generation_token = 0;
        std::uint64_t minimizer_sequence_spill_mutation_before_release = 0;
        std::uint64_t minimizer_sequence_spill_mutation_after_release = 0;
        bool minimizer_sequence_spill_resources_released = false;
    } result{
        .index_kind = is_fragment_index
            ? ready_minimizer_generation_index_kind::fragment
            : ready_minimizer_generation_index_kind::sequence,
        .selected_backend = backend,
    };
    try {
            scoped_forced_bounded_nthash_index_failure forced_bounded_failure;
            state->source_authorization.emplace(
                authorize_minimizer_generation_source_pg(
                    state->identity, sequence_rowno,
                    registered_source_snapshot));
            state->source =
                state->source_authorization->source_for_diagnostics();
            state->source_content_checksum = state->source_authorization
                ->source_content_checksum_for_diagnostics();
            if (state->source.sequence_rowno != sequence_rowno
                || !same_minimizer_generation_sequence_locator(state->source.locator, locator)
                || state->source.alphabet_code
                    != source_binding_type::alphabet_code()
                || state->source.storage_unit_byte_width
                    != source_binding_type::storage_unit_byte_width()
                || state->source.padding_letters
                    != source_binding.last_fragment_padding()
                || state->source.locator.logical_length
                    != sequence.bp_size()
                || state->source.locator.fragment_count
                    != locator.fragment_count) {
                ready_minimizer_error_as_cpp(
                    ERRCODE_DATA_CORRUPTED,
                    "READY minimizer authoritative source authorization "
                    "disagrees with the passed lazy sequence");
            }
            state->source_authorization.reset();

            // Source identity is the immutable source epoch. The storage
            // contract is derived from the encoder's final manifest because the
            // planner may legitimately retry with a smaller fixed-width span.
            result.source_identity = minimizer_generation_source_identity(
                relation_identity.relfilenode, state->source);
            result.source_content_checksum =
                state->source_content_checksum;
            if (result.source_identity == 0) {
                ready_minimizer_error_as_cpp(
                    ERRCODE_DATA_CORRUPTED,
                    "READY minimizer source identity is zero");
            }

			ensure_vulkan_index_build_runtime_ready(backend);
            result.generation_id = bioseq::postgres::support::invoke_pg_as_cpp(
                [] { return allocate_minimizer_generation_id_pg(); });
            state->writer.emplace(storage.begin_generation_write(
                sequence_rowno, result.generation_id));
            forced_bounded_failure.arm_after_publisher_state_begin(backend);
            const bioseq::indexing::grouped_soa_generation_options options{
                .layout = bioseq::indexing::
                    minimizer_generation_layout::
                        grouped_soa_segment_local,
                .generation_id = result.generation_id,
                .source_identity = result.source_identity,
                .source_content_checksum =
                    state->source_content_checksum,
                .target_length = locator.logical_length,
                .position_segment_span_bp = persisted_segment_span_bp,
                .alphabet_code = state->source.alphabet_code,
                .storage_unit_byte_width =
                    state->source.storage_unit_byte_width,
                .kmer_size = k_kmer_size,
                .window_size = k_window_size,
                .minimizer_scheme_id = scheme.id,
                .minimizer_scheme_revision = scheme.revision,
                .max_occ_cap = k_max_occ_cap,
            };
            if (is_fragment_index) {
                auto encoded = fragment_index::encode_persisted_generation(
                    sequence, backend, kernels, *state->writer, options,
                    bioseq::indexing::grouped_soa_generation_limits{},
                    &state->fragment_execution,
                    state->source.alphabet_code);
                if (encoded) {
                    state->generation.emplace(std::move(*encoded));
                    state->execution = state->fragment_execution;
                } else {
                    state->encode_error = encoded.error();
                }
            } else {
                state->sequence_spill_started = true;
                auto spill = sequence_index::spill_generation_replay_runs_serial(
                    storage, sequence, backend, kernels,
                    /*core_span_bp=*/0,
                    bioseq::indexing::k_minimizer_preferred_segment_span_bp,
                    &state->sequence_execution,
                    state->source.alphabet_code);
                if (!spill) {
                    state->encode_error = spill.error();
                } else {
                    auto encoded = sequence_index::encode_persisted_generation(
                        storage, *state->writer, std::move(*spill), options,
                        bioseq::indexing::grouped_soa_generation_limits{},
                        &state->sequence_execution);
                    if (encoded) {
                        state->generation.emplace(std::move(*encoded));
                        state->execution = state->sequence_execution;
                    } else {
                        state->encode_error = encoded.error();
                    }
                }
            }
            if (state->generation.has_value()) {
                result.binding = bioseq::indexing::
                    make_minimizer_generation_publication_binding(
                        *state->generation);
            }

            // PostgreSQL owns every sequence-index spill run. Destroy those BufFile/run
            // resources after the encoder has copied out its immutable validation state,
            // and before any READY token can be validated or published.
            if (is_sequence_index) {
                const auto before_release =
                    storage.segment_spill_storage_binding();
                if (!state->encode_error
                    && (!state->generation.has_value()
                        || !state->sequence_execution
                            .allows_ready_release(before_release))) {
                    ready_minimizer_error_as_cpp(
                        ERRCODE_DATA_CORRUPTED,
                        "READY minimizer sequence spill incarnation changed before release");
                }
                storage.release_runs();
                const auto after_release =
                    storage.segment_spill_storage_binding();
                std::uint64_t retained_groups = 0;
                storage.merge_segment_runs(
                    [&retained_groups](auto&&...) { ++retained_groups; });
                result.minimizer_sequence_spill_generation_token =
                    before_release.generation_token;
                result.minimizer_sequence_spill_mutation_before_release =
                    before_release.mutation_revision;
                result.minimizer_sequence_spill_mutation_after_release =
                    after_release.mutation_revision;
                result.minimizer_sequence_spill_resources_released =
                    retained_groups == 0
                    && before_release.valid()
                    && after_release.generation_token
                        == before_release.generation_token
                    && before_release.mutation_revision
                        != (std::numeric_limits<std::uint64_t>::max)()
                    && after_release.mutation_revision
                        == before_release.mutation_revision + 1u;
                state->sequence_spill_started = false;
            }
            if (state->encode_error) {
                if (backend == bioseq::minimizers::
                        minimizer_sketch_backend::bounded_nthash_simd
                    && *state->encode_error
                        == bioseq::indexing::minimizer_generation_encoding_error::
                            admission) {
                    throw bioseq::indexing::
                        packed_bounded_nthash_index_runtime_error{
                            bioseq::indexing::packed_bounded_nthash_index_status::
                                admission_denied,
                            is_fragment_index
                                ? "minimizer fragment generation encode"
                                : "minimizer sequence generation encode"};
                }
                ready_minimizer_error_as_cpp(
                    ERRCODE_DATA_CORRUPTED,
                    "READY minimizer generation encoder failed",
                    fmt::format("backend={} wire_error={}",
                        bioseq::minimizers::
                            minimizer_sketch_backend_name(backend),
                        static_cast<unsigned>(*state->encode_error)));
            }
            bool execution_rejected = false;
            std::uint64_t producer_scans = 0;
            std::uint64_t geometry_retries = 0;
            std::uint64_t scalar_runs = 0;
            std::uint64_t exact_runs = 0;
            std::uint64_t bounded_runs = 0;
            std::uint64_t vulkan_runs = 0;
            std::uint64_t vulkan_roundtrips = 0;
            if (const auto* execution = std::get_if<
                    bioseq::indexing::minimizer_fragment_generation_execution_receipt>(
                        &state->execution)) {
                execution_rejected = !execution->publication_accepted(
                        result.binding)
                    || !is_fragment_index
                    || execution->selected_backend != backend
                    || execution->minimizer_scheme != scheme
                    || execution->target_length != locator.logical_length
                    || execution->window_size != k_window_size
                    || execution->kmer_size != k_kmer_size;
                producer_scans = execution->producer_scan_count;
                geometry_retries = execution->geometry_retry_count;
                scalar_runs = execution->scalar_core_runs;
                exact_runs = execution->exact_simd_core_runs;
                bounded_runs = execution->bounded_nthash_core_runs;
                vulkan_runs = execution->vulkan_core_runs;
                vulkan_roundtrips =
                    execution->vulkan_stats.completed_roundtrips;
            } else if (const auto* execution = std::get_if<
                    bioseq::indexing::minimizer_sequence_generation_execution_receipt>(
                        &state->execution)) {
                execution_rejected = !execution->matches_publication(
                        result.binding)
                    || !is_sequence_index
                    || execution->selected_backend != backend
                    || execution->minimizer_scheme != scheme
                    || execution->target_length != locator.logical_length
                    || execution->window_size != k_window_size
                    || execution->kmer_size != k_kmer_size
                    || !result.minimizer_sequence_spill_resources_released;
                producer_scans = execution->replay_scan_count;
                geometry_retries = execution->replay_reroute_pass_count;
                scalar_runs = execution->scalar_core_runs;
                exact_runs = execution->exact_simd_core_runs;
                bounded_runs = execution->bounded_nthash_core_runs;
                vulkan_runs = execution->vulkan_core_runs;
                vulkan_roundtrips =
                    execution->vulkan_stats.completed_roundtrips;
            } else {
                execution_rejected = true;
            }
            if (execution_rejected) {
                ready_minimizer_error_as_cpp(
                    ERRCODE_DATA_CORRUPTED,
                    "READY minimizer selected backend execution stats were rejected",
                    fmt::format(
                        "backend={} scans={} retries={} scalar={} exact={} bounded={} vulkan={} vulkan_roundtrips={}",
                        bioseq::minimizers::
                            minimizer_sketch_backend_name(backend),
                        producer_scans, geometry_retries, scalar_runs,
                        exact_runs, bounded_runs, vulkan_runs,
                        vulkan_roundtrips));
            }
            const auto& manifest = state->generation->manifest;
            if (result.binding != bioseq::indexing::
                    make_minimizer_generation_publication_binding(
                        *state->generation)) {
                ready_minimizer_error_as_cpp(
                    ERRCODE_DATA_CORRUPTED,
                    "READY minimizer publication binding changed before validation");
            }
            result.storage_contract_checksum = bioseq::indexing::
                compute_minimizer_generation_storage_contract_checksum(manifest);
            state->source.storage_contract_checksum =
                result.storage_contract_checksum;
            if (manifest.layout != bioseq::indexing::
                    minimizer_generation_layout::
                        grouped_soa_segment_local
                || manifest.generation_id != result.generation_id
                || manifest.source_identity != result.source_identity
                || manifest.source_content_checksum
                    != state->source_content_checksum
                || manifest.target_length != locator.logical_length
                || manifest.alphabet_code != state->source.alphabet_code
                || manifest.storage_unit_byte_width
                    != state->source.storage_unit_byte_width
                || manifest.kmer_size != k_kmer_size
                || manifest.window_size != k_window_size
                || manifest.minimizer_scheme_id != scheme.id
                || manifest.minimizer_scheme_revision != scheme.revision
                || manifest.producer_backend_id != producer_backend.id
                || manifest.producer_identity_revision
                    != producer_backend.revision
                || manifest.gpu_abi_revision != profile.gpu_abi_revision
                || manifest.max_occ_cap != k_max_occ_cap
                || manifest.position_segment_span_bp == 0
                || (is_fragment_index
                    ? manifest.position_segment_span_bp
                        != k_fragment_span_bp
                    : manifest.position_segment_span_bp
                        > bioseq::indexing::
                            k_minimizer_preferred_segment_span_bp)
                || state->generation->head.storage_contract_checksum
                    != result.storage_contract_checksum
                || state->generation->head.source_identity
                    != result.source_identity
                || state->generation->head.source_content_checksum
                    != state->source_content_checksum) {
                ready_minimizer_error_as_cpp(
                    ERRCODE_DATA_CORRUPTED,
                    "READY minimizer encoded manifest identity disagrees");
            }

            state->ready_token.emplace(
                state->writer->validate_ready_generation(
                    *state->generation));
            result.writer_rows = state->writer->row_count();
            result.writer_batches =
                state->writer->catalog_multi_insert_batches();
            result.control_count = manifest.control_count;
            result.position_count = manifest.position_count;
            state->generation.reset();
            state->writer.reset();
            storage.close_checked();
            inject_minimizer_generation_failure_as_cpp(
                minimizer_generation_failure_injection::generation_storage_close,
                "generation_storage_close");

            ready_minimizer_generation_published_head_receipt publication_receipt =
                publish_ready_minimizer_generation_head_pg(
                    std::move(*state->ready_token));
            state->ready_token.reset();
            result.replaced_generation_id =
                publication_receipt.replaced_generation_id;

            bioseq::postgres::support::invoke_pg_as_cpp(
                [] { CommandCounterIncrement(); });
            bioseq::postgres::support::invoke_pg_as_cpp(
                [] { UpdateActiveSnapshotCommandId(); });
            Snapshot verification_source =
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [] { return GetActiveSnapshot(); });
            Snapshot verification_snapshot =
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [verification_source] {
                        return RegisterSnapshot(verification_source);
                    });
            const auto reread_expected = bioseq::indexing::
                expected_grouped_soa_generation_identity_for_backend(
                    bioseq::indexing::minimizer_generation_layout::
                        grouped_soa_segment_local,
                    backend, state->source.alphabet_code,
                    state->source.storage_unit_byte_width,
                    k_kmer_size, k_window_size, k_max_occ_cap);
            if (!reread_expected) {
                ready_minimizer_error_as_cpp(
                    ERRCODE_DATA_CORRUPTED,
                    "READY minimizer publication reread contract is invalid");
            }
            {
                index_storage_fragment_generation_read_session reader{
                    publication_receipt.table_name,
                    publication_receipt.query_key,
                    verification_snapshot,
                    *reread_expected,
                    state->identity.tablespace_oid,
                    minimizer_generation_snapshot_registration::borrow_registered};
                const minimizer_generation_head reread_head =
                    reader.read_active_head_once();
                const auto reread_head_bytes = bioseq::indexing::
                    encode_minimizer_generation_head(reread_head);
                if (!reread_head_bytes
                    || reread_head != publication_receipt.head
                    || !bioseq::indexing::is_fixed_wire_returned_exactly(
                        publication_receipt.head_bytes,
                        *reread_head_bytes)) {
                    ready_minimizer_error_as_cpp(
                        ERRCODE_DATA_CORRUPTED,
                        "READY minimizer publication independent head reread disagrees");
                }
                inject_minimizer_generation_failure_as_cpp(
                    minimizer_generation_failure_injection::generation_post_publish_reread,
                    "generation_post_publish_reread");
                reader.close_head_read();
                inject_minimizer_generation_failure_as_cpp(
                    minimizer_generation_failure_injection::generation_reader_close,
                    "generation_reader_close");
            }
            bioseq::postgres::support::invoke_pg_as_cpp(
                [verification_snapshot] {
                    UnregisterSnapshot(verification_snapshot);
                });

            result.execution = state->execution;
            delete state;
            state = nullptr;
    } catch (const bioseq::postgres::support::postgres_error&) {
        storage.abandon_after_pg_error_noexcept();
        state->ready_token.reset();
        state->generation.reset();
        state->sequence_spill_started = false;
        state->writer.reset();
        state->source_authorization.reset();
        delete state;
        state = nullptr;
        throw;
    } catch (...) {
        storage.abandon_after_pg_error_noexcept();
        state->ready_token.reset();
        state->generation.reset();
        state->sequence_spill_started = false;
        state->writer.reset();
        state->source_authorization.reset();
        delete state;
        state = nullptr;
        throw;
    }
    return ready_minimizer_generation_publication{
        result.index_kind,
        result.binding,
        result.control_count,
        result.position_count,
        result.writer_rows,
        result.writer_batches,
        result.selected_backend,
        std::move(result.execution),
        result.replaced_generation_id,
        result.minimizer_sequence_spill_generation_token,
        result.minimizer_sequence_spill_mutation_before_release,
        result.minimizer_sequence_spill_mutation_after_release,
        result.minimizer_sequence_spill_resources_released};
}

/// Seal the production update_index result at the caller boundary. The
/// coordinator already validates the producer-originated execution receipt before
/// publication; this bridge additionally validates that the returned alternative,
/// backend, and immutable publication binding still match the requested normal
/// rebuild route. There is no scalar/Exact fallback on any selected backend.
template <class p_sequence_range>
inline void build_and_verify_grouped_soa_minimizer_generation_pg(
        Oid user_relid,
        std::int16_t column_attno,
        std::string_view index_name,
        std::uint64_t sequence_rowno,
        fragment_index_storage& storage,
        bioseq::storage::lazy_sequence_view<
            typename bioseq::storage::postgres_lazy_sequence_binding<p_sequence_range>::
                alphabet_type> sequence,
        bioseq::storage::postgres_lazy_sequence_binding<p_sequence_range> source_binding,
        bioseq::minimizers::minimizer_sketch_backend backend,
        const bioseq::minimizers::minimizer_kernels* kernels) {
    const ready_minimizer_generation_publication publication =
        build_and_publish_grouped_soa_minimizer_generation_pg(
            user_relid, column_attno, index_name, sequence_rowno, storage,
            sequence, source_binding, backend, kernels);
    const bool execution_kind_matches =
        index_name == k_minimizers_per_fragment_index_name
            ? std::holds_alternative<
                bioseq::indexing::minimizer_fragment_generation_execution_receipt>(
                    publication.execution())
            : index_name == k_minimizers_per_sequence_index_name
                && std::holds_alternative<
                    bioseq::indexing::minimizer_sequence_generation_execution_receipt>(
                        publication.execution());
    const bool result_kind_matches =
        (index_name == k_minimizers_per_fragment_index_name
            && publication.index_kind()
                == ready_minimizer_generation_index_kind::fragment)
        || (index_name == k_minimizers_per_sequence_index_name
            && publication.index_kind()
                == ready_minimizer_generation_index_kind::sequence);
    if (!publication.published()
        || publication.generation_id() == 0
        || publication.source_identity() == 0
        || publication.selected_backend() != backend
        || !publication.execution_accepted()
        || !execution_kind_matches
        || !result_kind_matches) {
        ready_minimizer_error_as_cpp(
            ERRCODE_DATA_CORRUPTED,
            "normal minimizer rebuild publication validation was rejected",
            fmt::format("index={} backend={}", index_name,
                bioseq::minimizers::
                    minimizer_sketch_backend_name(backend)));
    }
}

// Keep one explicit production facade/binding instantiation as a narrow
// compile-time contract in addition to the ordinary update_index call sites.
namespace ready_minimizer_generation_compile_contract_detail {
using sequence_config = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
using source_binding =
    bioseq::storage::postgres_lazy_sequence_binding<sequence_config>;
using sequence_view = bioseq::storage::lazy_sequence_view<seq::dna4>;
}  // namespace ready_minimizer_generation_compile_contract_detail

template ready_minimizer_generation_publication
build_and_publish_grouped_soa_minimizer_generation_pg<
    ready_minimizer_generation_compile_contract_detail::sequence_config>(
        Oid,
        std::int16_t,
        std::string_view,
        std::uint64_t,
        fragment_index_storage&,
        ready_minimizer_generation_compile_contract_detail::sequence_view,
        ready_minimizer_generation_compile_contract_detail::source_binding,
        bioseq::minimizers::minimizer_sketch_backend,
        const bioseq::minimizers::minimizer_kernels*);

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

    [[nodiscard]] const ready_minimizer_generation_publication&
    sealed_publication() const noexcept {
        DEBUG_ASSERT(publication.has_value(),
            "READY fixture publication must be sealed before return");
        return *publication;
    }
};

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

    [[nodiscard]] bioseq::indexing::minimizer_scheme_identity
    scheme_identity() const noexcept {
        return bioseq::indexing::minimizer_scheme_identity_for_backend(
            backend);
    }

    [[nodiscard]] bioseq::indexing::minimizer_producer_identity
    producer_backend_identity() const noexcept {
        return bioseq::indexing::
            minimizer_producer_backend_identity_for_backend(backend);
    }

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
            bioseq::postgres::support::invoke_pg_as_cpp([&] {
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

struct cache_keys {
    index_chunk_key chunk{};
    assembled_index_key assembled{};
};

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
        const auto resolve_table_oid = [&] {
            return bioseq::postgres::support::invoke_pg_as_cpp([&] {
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
                bioseq::postgres::support::invoke_pg_as_cpp([&] {
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
            Relation restored = bioseq::postgres::support::invoke_pg_as_cpp([&] {
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

[[noreturn]] inline void index_audit_error(std::string_view message) {
    ereport(ERROR,
        (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
         errmsg("bioseq physical minimizer-index audit: %.*s",
            static_cast<int>(message.size()), message.data())));
    pg_unreachable();
}

inline void spi_connect_for_index_audit() {
    if (SPI_connect() != SPI_OK_CONNECT) {
        index_audit_error("SPI connection failed");
    }
}

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

[[nodiscard]] inline std::int64_t count_in_shard(
        const std::string& relname,
        const std::string& where_clause) {
    return index_audit_scalar_i64(fmt::format(
        "SELECT count(*)::bigint FROM bioseq_internal.{} {}",
        quote_identifier(relname.c_str()), where_clause));
}

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
        (unsigned)rel_oid, (unsigned)rid.relfilenode, quote_literal_cstr(std::string(name).c_str()),
        update_on_insert ? "true" : "false", (unsigned)rel_oid);
    SPI_execute(sql.c_str(), false, 0);
    SPI_finish();
    return 0;
}

[[nodiscard]] inline int set_active_read_index_impl(Oid rel_oid, std::string_view name) {
    if (!name_is_registered(name)) return 1;
    if (layout_of_name(name) == sequence_index_layout::table_chunks) return 2;
    const relation_identity rid = index_relation_identity(rel_oid);
    if (SPI_connect() != SPI_OK_CONNECT) return 0;
    std::string clear = fmt::format(
        "UPDATE bioseq_internal.index_policy SET is_active_read=false WHERE user_relid={}::oid AND main_relfilenode={}::oid",
        (unsigned)rel_oid, (unsigned)rid.relfilenode);
    SPI_execute(clear.c_str(), false, 0);
    std::string set_ = fmt::format(
        "INSERT INTO bioseq_internal.index_policy (user_relid, main_relfilenode, column_attno, index_name, enabled, is_active_read) "
        "SELECT {}::oid, {}::oid, a.attnum, {}, true, true FROM pg_attribute a JOIN pg_type t ON a.atttypid=t.oid "
        "JOIN pg_namespace n ON t.typnamespace=n.oid WHERE a.attrelid={}::oid AND t.typname='seq' AND n.nspname IN ('bioseq','bioseq_internal') "
        "AND a.attnum>0 AND NOT a.attisdropped "
        "ON CONFLICT (user_relid, main_relfilenode, column_attno, index_name) DO UPDATE SET is_active_read=true, enabled=true",
        (unsigned)rel_oid, (unsigned)rid.relfilenode, quote_literal_cstr(std::string(name).c_str()), (unsigned)rel_oid);
    SPI_execute(set_.c_str(), false, 0);
    SPI_finish();
    return 0;
}

}  // namespace bioseq::storage::indexing
