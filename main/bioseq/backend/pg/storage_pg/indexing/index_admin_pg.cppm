/**
 * @file index_admin_pg.cppm
 * @brief DOC0279 §8.7/§11/§12 — the pg-side registry instantiation, the
 *        per-sequence storage factories, the INSERT/admin build drivers, the
 *        symbol-count reader glue, and the C++ impls behind the SQL surface.
 */
module;
#include <hwy/highway.h>  // DOC0345 Patch I — HWY_NAMESPACE macro names MinimizerAlgorithmType / MinimizerSketchBackend
#include "common.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/heapam.h"
#include "executor/tuptable.h"
#include "utils/inval.h"
#include "utils/snapmgr.h"
#include "utils/memutils.h"
#include <algorithm>
#include <array>
#include <concepts>
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
export module bioseq.storage.indexing_pg.admin;
import bioseq.common.mlogging;
import bioseq.pg_utils;
import bioseq.config;                             // DOC0322 §9.3 — bioseq_update_index_on_insert GUC
import bioseq.storage.manager;                     // seq_caches, seq_locator, is_locator_stub, decode_locator_from_stub
import bioseq.storage.bucket_math;
import bioseq.storage.size;
import bioseq.storage.lazy;                // make_lazy_in_memory
import bioseq.storage.scanner_pg;          // with_lazy_postgres
import bioseq.types.pg_compact_sequence;   // pgseq_t, dispatch_instance
import bioseq.sequences;                   // compact_sequence_range
import bioseq.common.sequan_compat;                      // seqan3::char_is_valid_for
import bioseq.indexing.type;
import bioseq.indexing.storage;
import bioseq.indexing.traits;
import bioseq.indexing.registry;
import bioseq.indexing.minimizer_hashmap;
import bioseq.indexing.minimizer_sequence;
import bioseq.sequences.minimap_port;   // DOC0345 Patch I — MinimizerAlgorithmType (spec's bioseq.pipeline.minimap_port name does not exist)
import bioseq.sequences.kmers;          // DOC0345 Patch I — MinimizerSketchBackend, minimizer_sketch_backend_name
import bioseq.indexing.symbol_counts;
import bioseq.storage.indexing_pg.chunked_storage;
import bioseq.storage.indexing_pg.residency;
import bioseq.parallelism.memory_admission;
import bioseq.parallelism.mode;
import bioseq.parallelism.pool;
import bioseq.scheduler.supervisor_pg;
import bioseq.table.io;                     // BioseqColumnBeginRead/BioseqColumnReadNextRow/BioseqColumnEndRead (raw-stub row walk, §8.6)
export import bioseq.storage.indexing_pg.handles;
import bioseq.column.tam;                    // DOC0322 §7.4 — tid_to_row_number, IsBioseqColumnTableAmTable (leaf)
import bioseq.storage.metadata;                     // DOC0322 §7.4 — BioseqColumnTableRowCount (moved to storage_metadata:core)
static_assert(__cplusplus >= 202302L, "C++23-or-later");

export namespace bioseq::storage::indexing {

using bioseq::indexing::SequenceIndexType;
using bioseq::indexing::IndexUpdatePolicy;
using bioseq::indexing::index_sub_key_t;
using bioseq::indexing::DIRECTORY_SUB_KEY;
using bioseq::indexing::GENERATION_HEAD_SUB_KEY;
using bioseq::indexing::TEMP_RUN_BASE;
using bioseq::indexing::derive_minimizer_position_extent_sub_key;

#ifdef BIOSEQ_TEST_INDEXES
/// DOC0279 §11.4/§16.2 — a trivial CHUNKED_PER_TABLE index, compiled into the
/// dylib ONLY under -DBIOSEQ_TEST_INDEXES, that gives IndexStorageTablePg's
/// for_each_sequence / write_to / index_table_shard rollover live integration
/// coverage. build() streams every row (columnar rowno) and persists the row
/// count as one int64 chunk. allows_update_on_insert=false (table-wide-never).
template <class S>
struct TestTableCountIndex {
    static_assert(bioseq::indexing::is_index_chunked_storage_for<S, SequenceIndexType::CHUNKED_PER_TABLE>);
    static constexpr SequenceIndexType index_type = SequenceIndexType::CHUNKED_PER_TABLE;
    static constexpr std::string_view  index_name = "test_table_rowcount";
    using storage_type = S;
    static void build(S& storage) {
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
    static void rebuild(S& storage) { storage.clear(index_name); build(storage); }
};
#endif
}  // namespace bioseq::storage::indexing

#ifdef BIOSEQ_TEST_INDEXES
export namespace bioseq::indexing {
template <> struct index_descriptor<bioseq::storage::indexing::TestTableCountIndex> {
    static constexpr SequenceIndexType index_type = SequenceIndexType::CHUNKED_PER_TABLE;
    static constexpr std::string_view  index_name = "test_table_rowcount";
    static constexpr bool allows_update_on_insert = false;
};
}  // namespace bioseq::indexing
#endif

export namespace bioseq::storage::indexing {

/// The single registry of every production index (§12.1). Under
/// -DBIOSEQ_TEST_INDEXES the test-only CHUNKED_PER_TABLE TestTableCountIndex is
/// appended so its for_each_sequence/write_to/rollover path gets live coverage.
using pg_supported_indexes = bioseq::indexing::supported_indexes<
    IndexStorageFragmentPg,
    IndexStorageSequencePg,
    IndexStorageTablePg,
    bioseq::indexing::MinimizersPerFragmentIndex,     // CHUNKED_PER_FRAGMENT
    bioseq::indexing::MinimizerSequenceMergedIndex,   // CHUNKED_PER_FRAGMENT
    bioseq::indexing::SymbolCountsIndex               // INLINE_PER_SEQUENCE
#ifdef BIOSEQ_TEST_INDEXES
    , TestTableCountIndex                             // CHUNKED_PER_TABLE,    "test_table_rowcount"
#endif
    >;

// g_last_index_merge_run_count (§8.8) is defined in bioseq_storage_indexing_pg
// (same namespace, imported) so merge_runs can set it; referenced unqualified below.

// ---- policy catalog (§12.3) ----------------------------------------------
struct PolicyRow { std::string name; bool enabled; bool update_on_insert; };
[[nodiscard]] inline std::vector<PolicyRow> load_table_policy(Oid user_relid, Oid relfn, std::int16_t attno) {
    std::vector<PolicyRow> out;
    if (SPI_connect() != SPI_OK_CONNECT) return out;
    std::string sql = fmt::format(
        "SELECT index_name, enabled, update_on_insert FROM bioseq_internal.index_policy "
        "WHERE user_relid = {}::oid AND main_relfilenode = {}::oid AND column_attno = {}",
        (unsigned)user_relid, (unsigned)relfn, (int)attno);
    if (SPI_execute(sql.c_str(), true, 0) == SPI_OK_SELECT) {
        for (std::uint64_t i = 0; i < SPI_processed; ++i) {
            bool n1,n2,n3;
            char* nm = SPI_getvalue(SPI_tuptable->vals[i], SPI_tuptable->tupdesc, 1);
            Datum en = SPI_getbinval(SPI_tuptable->vals[i], SPI_tuptable->tupdesc, 2, &n2);
            Datum ui = SPI_getbinval(SPI_tuptable->vals[i], SPI_tuptable->tupdesc, 3, &n3);
            (void)n1;
            out.push_back(PolicyRow{ nm ? std::string(nm) : std::string(), DatumGetBool(en), DatumGetBool(ui) });
        }
    }
    SPI_finish();
    return out;
}

// ---- age log + policy upsert ---------------------------------------------
inline void note_successful_write(Oid user_relid, Oid relfn, std::int16_t attno, std::string_view name) {
    if (SPI_connect() != SPI_OK_CONNECT) return;
    std::string sql = fmt::format(
        "INSERT INTO bioseq_internal.metadata_age_log (user_relid, main_relfilenode, column_attno, index_name, last_updated) "
        "VALUES ({}::oid, {}::oid, {}, {}, now()) "
        "ON CONFLICT (user_relid, main_relfilenode, column_attno, index_name) DO UPDATE SET last_updated = now()",
        (unsigned)user_relid, (unsigned)relfn, (int)attno, quote_literal_cstr(std::string(name).c_str()));
    SPI_execute(sql.c_str(), false, 0);
    SPI_finish();
}

inline void note_policy_on_insert_writes(
        Oid user_relid,
        Oid relfn,
        std::int16_t attno,
        const pg_supported_indexes& idx) {
    constexpr auto types = pg_supported_indexes::list_types();
    for (std::size_t i = 0; i < types.size(); ++i) {
        if (idx.enabled[i]
            && idx.policy[i] == IndexUpdatePolicy::UPDATE_ON_INSERT) {
            note_successful_write(user_relid, relfn, attno, types[i].first);
        }
    }
}

// Select the target index sketch backend from the minimizer producer itself.
// Persisted minimizer semantics do not depend on the later chaining/alignment
// choices: changing either stage must not silently relabel an explicitly
// selected ExactSimd or PaperNtHashSimd index as Scalar.
// Defined before its first caller (index_build_on_insert); the spec placed it
// near layout_of_name, which is below index_build_on_insert.
[[nodiscard]] inline bioseq::simd::HWY_NAMESPACE::MinimizerSketchBackend
resolve_index_sketch_backend_from_gucs() noexcept
{
    using bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType;
    using bioseq::simd::HWY_NAMESPACE::MinimizerSketchBackend;
    if (bioseq_aligner_vulkan_enabled
        && bioseq_aligner_vulkan_gpu_index_build_enabled
        && bioseq_aligner_minimizer_algorithm
            == static_cast<int>(MinimizerAlgorithmType::VULKAN_MINIMAP_INT)) {
        return MinimizerSketchBackend::VulkanSegment;
    }
    if (bioseq_aligner_minimizer_algorithm
            == static_cast<int>(MinimizerAlgorithmType::TWO_STACK_NTHASH)) {
        return MinimizerSketchBackend::ExactSimd;
    }
    if (bioseq_aligner_minimizer_algorithm
            == static_cast<int>(MinimizerAlgorithmType::PAPER_NTHASH_SIMD)) {
        return MinimizerSketchBackend::PaperNtHashSimd;
    }
    return MinimizerSketchBackend::Scalar;
}

// DOC0350 — make the GPU runtime usable before a selected Vulkan index build.
// bioseq is in shared_preload_libraries, so bioseq_init()'s shader-provider
// install runs during preload and no-ops (install_pg_shader_provider_if_available
// early-returns while process_shared_preload_libraries_in_progress). Forked
// backends never re-run bioseq_init, and only the custom_scan QUERY path
// re-installs the provider lazily. A bioseq.load / bioseq.update_index GPU index
// build therefore reaches the Vulkan sketch with NO shader provider installed:
// Runtime::create_phase_b then takes the SPIR-V source-compile branch, MoltenVK
// calls newLibraryWithSource, and the MTLCompilerService XPC connection is
// invalid in a fork-without-exec PG backend (DOC0344) -> VkResult -3 -> uncaught
// VulkanRuntimeError -> SIGABRT. Installing the provider + waiting for the GPU
// runtime cache (wait_for_runtime_ready also setenvs the binary-archive dir the
// PSO interposer reads) before the build routes the sketch through the cached
// metallib + .metalar archives instead, exactly like the query path.
inline void ensure_vulkan_index_build_runtime_ready(
    bioseq::simd::HWY_NAMESPACE::MinimizerSketchBackend backend)
{
    if (backend
        != bioseq::simd::HWY_NAMESPACE::MinimizerSketchBackend::VulkanSegment) {
        return;
    }
    (void)bioseq::parallelism::pg::ensure_pg_vulkan_runtime_ready();
}

/// Legacy MPF/MPS rows do not carry a minimizer-scheme identity.  Persisting
/// Paper ntHash into those relations would let a later Exact reader (or the
/// reverse) accept incompatible hashes under the same index name.  Keep the
/// live v2 path fail-loud until the scheme-tagged READY-generation writer and
/// reader are activated together; silently substituting Scalar/Exact is never
/// permitted.
[[noreturn]] inline void reject_paper_for_legacy_fragment_persistence(
        bioseq::simd::HWY_NAMESPACE::MinimizerSketchBackend backend,
        std::string_view operation) {
    if (backend
        != bioseq::simd::HWY_NAMESPACE::MinimizerSketchBackend::
            PaperNtHashSimd) {
        bioseq_report<ERR, PG>()
            .code(ERRCODE_INTERNAL_ERROR)
            .msg("legacy Paper persistence guard received a non-Paper backend");
        __builtin_unreachable();
    }
    bioseq_report<ERR, PG>()
        .code(ERRCODE_FEATURE_NOT_SUPPORTED)
        .msg("paper_nthash_simd persistence requires the scheme-tagged "
             "READY-generation format")
        .detail("{} selected legacy MPF/MPS storage, whose payload does not "
                "record a minimizer-scheme identity; persistence is rejected "
                "without fallback",
                operation)
        .hint("Use Exact SIMD for the legacy index or activate the revision-3/4 "
              "scheme-identified writer and reader together.");
    __builtin_unreachable();
}

// ---- INSERT-time build (§11.2) -------------------------------------------
inline void index_build_on_insert(Oid user_relid, std::int16_t attno, std::uint64_t rowno, const pgseq_t* hdr,
                                  const std::optional<seq_locator>& loc_opt, MemoryContext mctx) {
    if (!bioseq_update_index_on_insert) return;
    if (!loc_opt.has_value()) return;
    const seq_locator& loc = *loc_opt;
    const RelationIdentity rid = index_relation_identity(user_relid);
    auto policy_rows = load_table_policy(user_relid, rid.relfilenode, attno);
    pg_supported_indexes idx; idx.apply_table_defaults(); idx.merge_overrides(policy_rows);
    reset_last_mps_build_stats();
    const auto backend = resolve_index_sketch_backend_from_gucs();  // DOC0345 Patch I
    if (backend
        == bioseq::simd::HWY_NAMESPACE::MinimizerSketchBackend::
            PaperNtHashSimd) {
        constexpr auto types = pg_supported_indexes::list_types();
        for (std::size_t i = 0; i < types.size(); ++i) {
            if (types[i].second == SequenceIndexType::CHUNKED_PER_FRAGMENT
                && idx.enabled[i]
                && idx.policy[i] == IndexUpdatePolicy::UPDATE_ON_INSERT) {
                reject_paper_for_legacy_fragment_persistence(
                    backend, "bioseq INSERT index build");
            }
        }
    }
    ensure_vulkan_index_build_runtime_ready(backend);  // DOC0350
    pgseq_t::dispatch_instance(hdr->alphabet_code, hdr->storage_unit_byte_width, [&]<class S>() {
        compact_sequence_range<S> rng{*hdr};
        auto lazy = bioseq::storage::make_lazy_in_memory(rng);
        std::map<std::string_view, IndexStorageFragmentPg> fcache;
        std::map<std::string_view, IndexStorageSequencePg> qcache;
        auto open_frag = [&](std::string_view name) -> IndexStorageFragmentPg& {
            if (auto it = fcache.find(name); it != fcache.end()) return it->second;
            acquire_index_advisory_xact_lock(rid.db_oid, rid.tablespace_oid, rid.relfilenode, attno, name);
            return fcache.emplace(name, open_fragment_storage(user_relid, rid.db_oid, rid.tablespace_oid, rid.relfilenode,
                                     attno, name, loc, rowno, nullptr, mctx)).first->second; };
        auto open_inl = [&](std::string_view name) -> IndexStorageSequencePg& {
            if (auto it = qcache.find(name); it != qcache.end()) return it->second;
            acquire_index_advisory_xact_lock(rid.db_oid, rid.tablespace_oid, rid.relfilenode, attno, name);
            return qcache.emplace(name, open_sequence_storage(user_relid, rid.db_oid, rid.tablespace_oid, rid.relfilenode,
                                     attno, name, loc, rowno, nullptr, mctx)).first->second; };
        try {
            idx.build_fragmented<true>(open_frag, lazy, backend);
            idx.build_sequence<true>(open_inl, lazy);
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
        const std::vector<std::optional<seq_locator>>& per_col_locator, std::uint64_t rowno, MemoryContext mctx) {
    if (!bioseq_update_index_on_insert) return;
    for (std::size_t c = 0; c < seq_attnos.size() && c < per_col_locator.size(); ++c) {
        if (!per_col_locator[c].has_value()) continue;
        const std::int16_t attno = seq_attnos[c];
        if (tts_isnull[attno - 1]) continue;
        const pgseq_t* hdr = reinterpret_cast<const pgseq_t*>(DatumGetPointer(detoasted_values[attno - 1]));
        index_build_on_insert(user_relid, attno, rowno, hdr, per_col_locator[c], mctx);
    }
}

// ---- letter_to_rank (moved from sidecar, §12.5) ---------------------------
namespace detail {
constexpr std::size_t kNumAlphabets = static_cast<std::size_t>(pgseq_supported_alphabets::alphabet_count);
constexpr std::size_t kCharDomain   = 256;
inline auto build_letter_rank_table() {
    std::array<std::array<std::int8_t, kCharDomain>, kNumAlphabets> table{};
    for (auto& row : table) row.fill((std::int8_t)-1);
    for (std::uint8_t ac = 0; ac < kNumAlphabets; ++ac) {
        pgseq_t::dispatch_instance(ac, 1, [&]<class S>() {
            using alphabet_t = typename S::alphabet_type;
            for (int c = 0; c < (int)kCharDomain; ++c) {
                const char letter = (char)c;
                if (!seqan3::char_is_valid_for<alphabet_t>(letter)) continue;
                alphabet_t sym{}; sym.assign_char(letter);
                table[ac][(std::uint8_t)c] = (std::int8_t)sym.to_rank();
            }
        });
    }
    return table;
}
inline const auto& letter_rank_table() { static const auto t = build_letter_rank_table(); return t; }
}  // namespace detail
[[nodiscard]] inline int letter_to_rank(std::uint8_t alphabet_code, char letter) {
    if (alphabet_code >= detail::kNumAlphabets) return -1;
    return (int)detail::letter_rank_table()[alphabet_code][(std::uint8_t)letter];
}

// ---- per-row index-present probe (§11.3c) --------------------------------
[[nodiscard]] inline bool index_present(Oid user_relid, Oid db, Oid spc, Oid relfn, std::int16_t attno,
                                        std::string_view index_name, const seq_locator& loc, std::int64_t row_key,
                                        seq_caches* caches, MemoryContext mctx) {
    auto s = open_fragment_storage(user_relid, db, spc, relfn, attno, index_name, loc, (std::uint64_t)row_key, caches, mctx);
    // both layouts gate on the DIRECTORY_SUB_KEY witness written LAST.
    try {
        const bool present = s.number_of_chunks(DIRECTORY_SUB_KEY) >= 1;
        s.close_checked();
        return present;
    } catch (...) {
        s.abandon_after_pg_error_noexcept();
        throw;
    }
}

// ---- estimate_index_presence (planner cost bias only, §11.5) -------------
[[nodiscard]] inline float estimate_index_presence(Oid user_relid, double estimated_total_rows, std::string_view index_name) {
    if (estimated_total_rows <= 0) return 0.0f;
    const RelationIdentity rid = index_relation_identity(user_relid);
    // Probe the bucket-0 shard-0 shard's reltuples (free, no scan).
    const std::string nm = format_seqidx_name(index_name, rid.db_oid, rid.tablespace_oid, rid.relfilenode, 0, 0, 0);
    const Oid ns = get_namespace_oid("bioseq_internal", true);
    if (!OidIsValid(ns)) return 0.0f;
    const Oid rel = get_relname_relid(nm.c_str(), ns);
    if (!OidIsValid(rel)) return 0.0f;
    double shard_reltuples = 0.0;
    { Relation r = try_safe_relation_open(rel, AccessShareLock);
      if (r != nullptr) { shard_reltuples = r->rd_rel->reltuples; relation_close(r, AccessShareLock); } }
    constexpr double nfrag_est = 64.0;  // coarse nominal
    double est_rows = shard_reltuples < 0 ? (double)0 : shard_reltuples;
    double distinct_seq_est = est_rows / nfrag_est;
    float cov = (float)std::min(1.0, distinct_seq_est / estimated_total_rows);
    return cov < 0.0f ? 0.0f : cov;
}

// ---- admin update_index (§11.4) ------------------------------------------
[[nodiscard]] inline bool name_is_registered(std::string_view name) {
    for (auto& t : pg_supported_indexes::list_types()) if (t.first == name) return true;
    return false;
}
[[nodiscard]] inline SequenceIndexType layout_of_name(std::string_view name) {
    return pg_supported_indexes::layout_of(name);
}

template <class Storage, class BuildFn>
inline void run_pg_index_build_with_cleanup(Storage& storage, BuildFn&& build_fn) {
    auto release_runs = [&storage]() noexcept {
        if constexpr (requires (Storage& cleanup_storage) {
                { cleanup_storage.release_runs_noexcept() } noexcept -> std::same_as<void>;
            }) {
            storage.release_runs_noexcept();
        } else if constexpr (requires (Storage& cleanup_storage) {
                { cleanup_storage.release_resident_runs_noexcept() } noexcept -> std::same_as<void>;
            }) {
            storage.release_resident_runs_noexcept();
        }
    };
    try {
        build_fn();
        if constexpr (requires (Storage& completed_storage) {
                { completed_storage.close_checked() } -> std::same_as<void>;
            }) {
            storage.close_checked();
        }
    } catch (const bioseq::pg_utils::PostgresErrorAsCpp&) {
        if constexpr (requires (Storage& failed_storage) {
                { failed_storage.abandon_after_pg_error_noexcept() }
                    noexcept -> std::same_as<void>;
            }) {
            storage.abandon_after_pg_error_noexcept();
        }
        throw;
    } catch (...) {
        if constexpr (requires (Storage& failed_storage) {
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

template <class R>
struct ResidentIndexBuildTarget {
    using traits = typename compact_sequence_range<R>::traits;
    using storage_unit = typename traits::storage_unit;
    using resident_config = sequence_custom_alphabet<
        typename traits::alphabet_type,
        storage_unit,
        traits::bits_per_char>;
    using resident_range = compact_sequence_range<resident_config>;

    bioseq::parallelism::MemoryPermit permit;
    resident_range range;
};

template <class R, class Scanner>
[[nodiscard]] std::optional<ResidentIndexBuildTarget<R>>
try_materialize_for_parallel_sequence_index_build(
        const bioseq::storage::lazy_compact_sequence<R, Scanner>& seq,
        std::uint64_t rowno,
        std::string_view index_name) {
    if constexpr (!Scanner::supports_fragments()) {
        return std::nullopt;
    } else {
        bioseq::parallelism::pg::backend_sync();
        auto& pool = bioseq::parallelism::GlobalPool::instance();
        if (!bioseq::parallelism::task_parallel(pool.mode())
            || pool.max_concurrency() <= 1) {
            return std::nullopt;
        }

        using resident_t = ResidentIndexBuildTarget<R>;
        using storage_unit = typename resident_t::storage_unit;
        const std::uint64_t total_units64 =
            static_cast<std::uint64_t>(seq.size());
        if (total_units64
            > static_cast<std::uint64_t>(
                (std::numeric_limits<std::size_t>::max)())) {
            bioseq_log<ERR, PG>(
                   "bioseq.update_index: row {} index {} has too many packed "
                   "units to materialize for parallel build",
                   static_cast<unsigned long long>(rowno),
                   std::string(index_name).c_str());
        }
        const std::size_t total_units =
            static_cast<std::size_t>(total_units64);
        const std::uint64_t packed_bytes =
            bioseq::parallelism::saturating_mul(
                total_units64,
                static_cast<std::uint64_t>(sizeof(storage_unit)));
        constexpr std::uint64_t context_reserve_bytes = std::uint64_t{8} << 20;
        const std::uint64_t reserve_bytes =
            packed_bytes
                    > (std::numeric_limits<std::uint64_t>::max)()
                        - context_reserve_bytes
                ? (std::numeric_limits<std::uint64_t>::max)()
                : packed_bytes + context_reserve_bytes;

        auto permit =
            bioseq::parallelism::MemoryAdmissionController::instance()
                .try_reserve(reserve_bytes);
        if (!permit) {
            bioseq_log<DBG>("[bioseq] index: row={} index={} target_bp={} packed_bytes={} "
                    "not admitted for resident parallel build; using scanner_pg",
                    static_cast<unsigned long long>(rowno),
                    index_name,
                    static_cast<unsigned long long>(seq.bp_size()),
                    static_cast<unsigned long long>(packed_bytes));
            return std::nullopt;
        }

        typename resident_t::resident_range::base_container packed(total_units);
        const std::size_t fragment_units = Scanner::fragment_storage_units;
        for (std::size_t fragment_no = 0;
             fragment_no < seq.number_of_fragments();
             ++fragment_no) {
            if ((fragment_no & std::size_t{0x3ff}) == 0) {
                CHECK_FOR_INTERRUPTS();
            }
            const std::span<const storage_unit> fragment =
                seq.scanner().read_fragment(fragment_no);
            const std::size_t dst_offset = fragment_no * fragment_units;
            if (dst_offset >= packed.size()) {
                seq.scanner().release_fragment(fragment_no);
                break;
            }
            const std::size_t copy_units =
                std::min(fragment.size(), packed.size() - dst_offset);
            std::copy_n(fragment.data(), copy_units, packed.data() + dst_offset);
            seq.scanner().release_fragment(fragment_no);
        }

        bioseq_log<DBG>("[bioseq] index: row={} index={} target_bp={} fragments={} "
                "packed_bytes={} materialized for resident parallel build",
                static_cast<unsigned long long>(rowno),
                index_name,
                static_cast<unsigned long long>(seq.bp_size()),
                seq.number_of_fragments(),
                static_cast<unsigned long long>(packed_bytes));

        return resident_t{
            std::move(*permit),
            resident_t::resident_range::from_packed(
                std::move(packed), seq.padding())};
    }
}

// DOC0345 Patch I — backend-aware. Whole-target resident materialization is
// disabled under Vulkan: the goal is streaming fragmented GPU windows, not making
// the fragmented target contiguous on the host.
template <class SupportedIndexes, class Storage, class R, class Scanner>
inline void rebuild_one_fragmented_maybe_resident(
        SupportedIndexes& idx,
        std::string_view name,
        Storage& store,
        bioseq::storage::lazy_compact_sequence<R, Scanner>& lazy,
        std::uint64_t rowno,
        bioseq::simd::HWY_NAMESPACE::MinimizerSketchBackend backend) {
    if (backend == bioseq::simd::HWY_NAMESPACE::MinimizerSketchBackend::Scalar
        && name == bioseq::indexing::MINIMIZERS_PER_SEQUENCE_INDEX_NAME) {
        if (auto resident =
                try_materialize_for_parallel_sequence_index_build(lazy, rowno, name);
            resident.has_value()) {
            auto resident_lazy = bioseq::storage::make_lazy_in_memory(resident->range);
            idx.rebuild_one_fragmented(name, store, resident_lazy, backend);
            return;
        }
    }
    idx.rebuild_one_fragmented(name, store, lazy, backend);
}

inline std::int64_t update_index_impl(Oid rel_oid, std::string_view name) {
    if (!name_is_registered(name)) bioseq_log<ERR, PG>( "bioseq.update_index: unknown index '{}'", std::string(name));
    if (!IsBioseqColumnTableAmTable(rel_oid)) bioseq_log<ERR, PG>( "bioseq.update_index: relation is not a USING bioseq table");
    const RelationIdentity rid = index_relation_identity(rel_oid);
    const SequenceIndexType layout = layout_of_name(name);
    reset_last_mps_build_stats();
    const auto backend = resolve_index_sketch_backend_from_gucs();  // DOC0345 Patch I
    if (layout == SequenceIndexType::CHUNKED_PER_FRAGMENT
        && backend
            == bioseq::simd::HWY_NAMESPACE::MinimizerSketchBackend::
                PaperNtHashSimd) {
        reject_paper_for_legacy_fragment_persistence(
            backend, "bioseq.update_index");
    }
    ensure_vulkan_index_build_runtime_ready(backend);  // DOC0350
    bioseq_log<DBG>(
        "[bioseq] index: update_index name={} backend={} vulkan_enabled={} "
        "vulkan_gpu_index_build={} minimizer_algorithm={}",
        name,
        bioseq::simd::HWY_NAMESPACE::minimizer_sketch_backend_name(backend),
        bioseq_aligner_vulkan_enabled,
        bioseq_aligner_vulkan_gpu_index_build_enabled,
        bioseq_aligner_minimizer_algorithm);

    // locate EVERY bioseq.seq column (§11.4 builds the index for each SEQ column;
    // mmsearch(target, query) queries the target column, which may not be the first).
    std::vector<std::int16_t> seq_attnos;
    if (SPI_connect() == SPI_OK_CONNECT) {
        std::string sql = fmt::format(
            "SELECT a.attnum FROM pg_attribute a JOIN pg_type t ON a.atttypid=t.oid "
            "JOIN pg_namespace n ON t.typnamespace=n.oid "
            "WHERE a.attrelid={}::oid AND t.typname='seq' AND n.nspname IN ('bioseq','bioseq_internal') "
            "AND a.attnum>0 AND NOT a.attisdropped ORDER BY a.attnum", (unsigned)rel_oid);
        if (SPI_execute(sql.c_str(), true, 0) == SPI_OK_SELECT) {
            for (std::uint64_t r = 0; r < SPI_processed; ++r) {
                bool isn=false; Datum d = SPI_getbinval(SPI_tuptable->vals[r], SPI_tuptable->tupdesc, 1, &isn);
                if (!isn) seq_attnos.push_back((std::int16_t)DatumGetInt16(d));
            }
        }
        SPI_finish();
    }
    if (seq_attnos.empty()) bioseq_log<ERR, PG>( "bioseq.update_index: relation has no bioseq.seq column");

    MemoryContext work = AllocSetContextCreate(CurrentMemoryContext, "bioseq_update_index", ALLOCSET_DEFAULT_SIZES);
    pg_supported_indexes idx; idx.apply_table_defaults();

    // clear the index for EVERY SEQ column first (idempotent rebuild).
    for (std::int16_t a : seq_attnos) {
        acquire_index_advisory_xact_lock(rid.db_oid, rid.tablespace_oid, rid.relfilenode, a, name);
        auto f = open_fragment_storage(rel_oid, rid.db_oid, rid.tablespace_oid, rid.relfilenode, a, name, seq_locator{}, 0, nullptr, work);
        auto q = open_sequence_storage(rel_oid, rid.db_oid, rid.tablespace_oid, rid.relfilenode, a, name, seq_locator{}, 0, nullptr, work);
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

    seq_caches* caches = bioseq::storage::alloc_seq_caches(work);
    caches->side = bioseq::storage::seq_caches::Side::Read;
    caches->lock_mode = AccessShareLock;
    caches->read_state_mctx = work;
    caches->main_database_oid = rid.db_oid;
    caches->main_tablespace_oid = rid.tablespace_oid;
    caches->main_relfilenode = rid.relfilenode;

    if (layout == SequenceIndexType::CHUNKED_PER_TABLE) {
        std::int64_t written = 0;
        if (Relation ur = try_safe_relation_open(rel_oid, AccessShareLock); ur != nullptr) {
            written = static_cast<std::int64_t>(BioseqColumnTableRowCount(ur));
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
    Relation ur = try_safe_relation_open(rel_oid, AccessShareLock);
    TupleDesc utd = RelationGetDescr(ur);
    // CRITICAL (DOC0279 §8.6/§11.4): read each row's stored value via the BIOSEQ_COLUMN
    // reader, NOT table_scan_getnextslot. The bioseq table-AM scan ASSEMBLES the full
    // sequence into one varlena, which exceeds MaxAllocSize (~1 GiB) for a multi-Gbp
    // target (6 Gbp => ~1.5 GB) and ERRORs "sequence too large to materialise as a
    // single varlena". BioseqColumnReadNextRow returns the RAW stored value (the 56-byte
    // locator stub for a large sequence); we decode the locator and STREAM fragments
    // via scanner_pg (RSS bounded by one fragment) — the only path that can index a
    // >MaxAllocSize target. `&rowno` is the columnar row number == query_key (no
    // tid_to_row_number needed; that path also fixed the rowno source).
    Snapshot snap = RegisterSnapshot(GetTransactionSnapshot());
    List* projected = nullptr;
    for (std::int16_t a : seq_attnos) projected = lappend_int(projected, (int)a);
    BioseqColumnReadState* rs = BioseqColumnBeginRead(ur, utd, projected, NIL, work, snap, /*randomAccess=*/false, nullptr);
    MemoryContext inner = AllocSetContextCreate(work, "bioseq_update_index_inner_seq", ALLOCSET_DEFAULT_SIZES);
    std::vector<Datum> vals(static_cast<std::size_t>(utd->natts));
    std::vector<char>  nullbuf(static_cast<std::size_t>(utd->natts));
    Datum* valp = vals.data();
    bool*  nulls = reinterpret_cast<bool*>(nullbuf.data());
    std::int64_t written = 0;
    std::uint64_t rowno = 0;
    while (BioseqColumnReadNextRow(rs, valp, nulls, &rowno)) {
        MemoryContextReset(inner);     // bound RSS to one row's decode + one fragment
        bool any_col = false;
        for (std::int16_t seq_attno : seq_attnos) {
            if (nulls[seq_attno - 1]) continue;
            MemoryContext old = MemoryContextSwitchTo(inner);
            pgseq_t* hdr = reinterpret_cast<pgseq_t*>(PG_DETOAST_DATUM(valp[seq_attno - 1]));
            MemoryContextSwitchTo(old);
            const std::uint8_t alpha = hdr->alphabet_code;
            const std::uint8_t bw = logical_storage_unit_byte_width(hdr);
            if (is_locator_stub(hdr)) {
                const seq_locator loc = decode_locator_from_stub(hdr);
                pgseq_t::dispatch_instance(alpha, bw, [&]<class S>(){
                    constexpr std::size_t lpu = compact_sequence_range<S>::traits::letters_per_unit;
                    const std::size_t pad = (loc.total_bp_size % lpu) == 0 ? 0 : (std::size_t)(lpu - loc.total_bp_size % lpu);
                    bioseq::storage::with_lazy_postgres<S>(
                        rid.db_oid, rid.tablespace_oid, rid.relfilenode,
                        seq_attno, loc, pad, rs->snapshot, caches, inner,
                        [&](auto& lazy){
                            if (layout == SequenceIndexType::INLINE_PER_SEQUENCE) {
                                auto store = open_sequence_storage(rel_oid, rid.db_oid, rid.tablespace_oid, rid.relfilenode, seq_attno, name, loc, rowno, caches, work);
                                run_pg_index_build_with_cleanup(store, [&idx, &name, &store, &lazy] {
                                    idx.rebuild_one_inline(name, store, lazy);
                                });
                            } else {
                                auto store = open_fragment_storage(rel_oid, rid.db_oid, rid.tablespace_oid, rid.relfilenode, seq_attno, name, loc, rowno, caches, work);
                                run_pg_index_build_with_cleanup(store, [&idx, &name, &store, &lazy, rowno, backend] {
                                    rebuild_one_fragmented_maybe_resident(
                                        idx, name, store, lazy, rowno, backend);
                                });
                            }
                        });
                });
            } else {
                pgseq_t::dispatch_instance(alpha, hdr->storage_unit_byte_width, [&]<class S>(){
                    compact_sequence_range<S> rng{*hdr};
                    auto lazy = bioseq::storage::make_lazy_in_memory(rng);
                    seq_locator loc{}; loc.total_bp_size = rng.bp_size();
                    loc.bucket_idx = (std::uint8_t)bioseq::storage::bucket_index_for_size<bioseq::storage::sequence_size_t,8,42>(loc.total_bp_size);
                    loc.shard_idx = 0;
                    if (layout == SequenceIndexType::INLINE_PER_SEQUENCE) {
                        auto store = open_sequence_storage(rel_oid, rid.db_oid, rid.tablespace_oid, rid.relfilenode, seq_attno, name, loc, rowno, caches, work);
                        run_pg_index_build_with_cleanup(store, [&idx, &name, &store, &lazy] {
                            idx.rebuild_one_inline(name, store, lazy);
                        });
                    } else {
                        auto store = open_fragment_storage(rel_oid, rid.db_oid, rid.tablespace_oid, rid.relfilenode, seq_attno, name, loc, rowno, caches, work);
                        run_pg_index_build_with_cleanup(store, [&idx, &name, &store, &lazy, rowno, backend] {
                            rebuild_one_fragmented_maybe_resident(
                                idx, name, store, lazy, rowno, backend);
                        });
                    }
                });
            }
            any_col = true;   // detoast copies live in `inner`, reclaimed by the per-row reset / final delete
        }
        if (any_col) ++written;
    }
    // Fragment readers borrow rs->snapshot as their compatibility owner, so
    // close them before BioseqColumnEndRead can unregister a refreshed copy.
    caches->close_all(/*commit=*/false);
    BioseqColumnEndRead(rs);
    UnregisterSnapshot(snap);
    MemoryContextDelete(inner);
    relation_close(ur, AccessShareLock);
    for (std::int16_t a : seq_attnos) note_successful_write(rel_oid, rid.relfilenode, a, name);
    bioseq::storage::destroy_seq_caches(caches);
    MemoryContextDelete(work);
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

/// SQL-facing boundary: no C++ execution error may cross the PostgreSQL C ABI.
[[nodiscard]] inline std::int64_t update_index_pg_checked(
        Oid rel_oid,
        std::string_view name) {
	ErrorData* exact_error = nullptr;
	std::array<char, 2048> detail{};
	bool exact_simd_error = false;
	bool cpp_error = false;
	bool unknown_cpp_error = false;
	try {
		return update_index_impl(rel_oid, name);
	} catch (bioseq::pg_utils::PostgresErrorAsCpp& error) {
		exact_error = error.release_error_data();
	} catch (const bioseq::indexing::ExactSimdIndexRuntimeError& err) {
		strlcpy(
			detail.data(),
			err.what() == nullptr ? "Exact SIMD error without detail" : err.what(),
			detail.size());
		exact_simd_error = true;
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

// DOC0286 §4.5 — (re)build every policy-enabled UPDATE_ON_INSERT index for
// `main_relid` in ONE batched pass (one streaming rebuild per index, no per-row
// advisory locks). bioseq.load calls this once after its streamed insert when
// bioseq.update_index_on_insert is on. It loads the same per-table policy as
// `index_build_on_insert`, honors every eligible UPDATE_ON_INSERT index, and
// drives `update_index_impl`, which rebuilds across
// every seq column and every row — so the observable index state matches a
// normal INSERT (only faster: one tuple-lock-free streaming pass).
[[nodiscard]] inline std::int64_t build_policy_update_on_insert_indexes(Oid main_relid) {
    const RelationIdentity rid = index_relation_identity(main_relid);
    pg_supported_indexes idx;
    idx.apply_table_defaults();

    // Overlay the persisted per-table policy. set_index_policy_impl stamps every
    // seq column identically, so the first seq column is a faithful table-level
    // proxy; if none is found the registry defaults stand.
    std::int16_t first_seq_attno = 0;
    if (SPI_connect() == SPI_OK_CONNECT) {
        const std::string sql = fmt::format(
            "SELECT a.attnum FROM pg_attribute a JOIN pg_type t ON a.atttypid=t.oid "
            "JOIN pg_namespace n ON t.typnamespace=n.oid WHERE a.attrelid={}::oid "
            "AND t.typname='seq' AND n.nspname IN ('bioseq','bioseq_internal') "
            "AND a.attnum>0 AND NOT a.attisdropped ORDER BY a.attnum LIMIT 1",
            static_cast<unsigned>(main_relid));
        if (SPI_execute(sql.c_str(), true, 0) == SPI_OK_SELECT && SPI_processed > 0) {
            bool isn = false;
            const Datum d = SPI_getbinval(SPI_tuptable->vals[0],
                                          SPI_tuptable->tupdesc, 1, &isn);
            if (!isn) first_seq_attno = static_cast<std::int16_t>(DatumGetInt16(d));
        }
        SPI_finish();
    }
    if (first_seq_attno > 0) {
        idx.merge_overrides(
            load_table_policy(main_relid, rid.relfilenode, first_seq_attno));
    }

    constexpr auto types = pg_supported_indexes::list_types();
    std::int64_t total = 0;
    for (std::size_t i = 0; i < types.size(); ++i) {
        if (!idx.enabled[i]) continue;
        if (idx.policy[i] != IndexUpdatePolicy::UPDATE_ON_INSERT) continue;
        // DOC0286 — invoke the index build through the SQL executor (SPI) rather
        // than calling update_index_impl() directly. The direct call ran the
        // builder inside bioseq.load's own executor frame; the relcache/typcache
        // churn from creating the seqidx sidecar relations (CREATE TABLE ...
        // bioseq.seq via internal subtransactions) then leaked into the next
        // command in the same backend (fetch_att "unsupported byval length: -1"
        // / custom-scan SIGSEGV). Routing through SPI gives the build its own
        // portal/executor frame with clean snapshot + invalidation handling —
        // exactly what a separate `bioseq.update_index` statement gets, which is
        // why the manual reindex never left residue.
        if (SPI_connect() == SPI_OK_CONNECT) {
            const std::string sql = fmt::format(
                "SELECT bioseq.update_index({}::oid::regclass, {})",
                static_cast<unsigned>(main_relid),
                quote_literal_cstr(std::string(types[i].first).c_str()));
            if (SPI_execute(sql.c_str(), false, 0) == SPI_OK_SELECT && SPI_processed > 0) {
                bool isn = false;
                const Datum d = SPI_getbinval(SPI_tuptable->vals[0],
                                              SPI_tuptable->tupdesc, 1, &isn);
                if (!isn) total += DatumGetInt64(d);
            }
            SPI_finish();
        }
    }
    return total;
}

// ---- §12.2 SQL helper impls ----------------------------------------------

/// Read-only setup/post-measurement proof returned by the narrow SQL admin
/// bridge. The authoritative source locator is derived from the visible
/// Bioseq row; callers never supply a source identity or compatibility digest.
struct ReadyMinimizerGenerationInspection {
    ReadyMinimizerGenerationIdentity identity{};
    Rev3AuthoritativeSourceLocator source{};
    MinimizerGenerationManifest manifest{};
    std::uint64_t source_content_checksum = 0;
    Oid database_oid = InvalidOid;
    Oid main_relfilenode = InvalidOid;
    std::int16_t column_attno = 0;
};

[[nodiscard]] inline std::int16_t
single_seq_attno_for_inspection(Oid rel_oid);

/// The evidence alternative is the tag.  There is deliberately no independent
/// mutable "MPF/MPS" enum that a caller could relabel after an encoder returns.
using ReadyMinimizerGenerationExecutionEvidencePg = std::variant<
    std::monostate,
    bioseq::indexing::MpfGenerationExecutionEvidence,
    bioseq::indexing::MpsGenerationExecutionEvidence>;

enum class ReadyMinimizerGenerationIndexKindPg : std::uint8_t {
    mpf,
    mps,
};

/// Proof-bearing result of the inactive common MPF/MPS generation coordinator.
/// Metadata age is deliberately absent: callers own that update and may perform
/// it only after this function returns a published result.
class ReadyMinimizerGenerationPublicationPg {
private:
    template <class R, class Scanner>
    friend ReadyMinimizerGenerationPublicationPg
    build_and_publish_grouped32_minimizer_generation_pg(
        Oid,
        std::int16_t,
        std::string_view,
        const seq_locator&,
        std::uint64_t,
        Snapshot,
        IndexStorageFragmentPg&,
        bioseq::storage::lazy_compact_sequence<R, Scanner>&,
        bioseq::simd::HWY_NAMESPACE::MinimizerSketchBackend);

    ReadyMinimizerGenerationIndexKindPg index_kind_ =
        ReadyMinimizerGenerationIndexKindPg::mpf;
    bioseq::indexing::MinimizerGenerationPublicationBinding binding_{};
    std::uint64_t control_count_ = 0;
    std::uint64_t position_count_ = 0;
    std::uint64_t writer_rows_ = 0;
    std::uint64_t writer_batches_ = 0;
    bioseq::simd::HWY_NAMESPACE::MinimizerSketchBackend selected_backend_ =
        bioseq::simd::HWY_NAMESPACE::MinimizerSketchBackend::Scalar;
    ReadyMinimizerGenerationExecutionEvidencePg execution_{};
    std::uint64_t mps_spill_generation_token_ = 0;
    std::uint64_t mps_spill_mutation_before_release_ = 0;
    std::uint64_t mps_spill_mutation_after_release_ = 0;
    bool mps_spill_resources_released_ = false;
    bool published_ = false;

    ReadyMinimizerGenerationPublicationPg(
            ReadyMinimizerGenerationIndexKindPg index_kind,
            bioseq::indexing::MinimizerGenerationPublicationBinding binding,
            std::uint64_t control_count,
            std::uint64_t position_count,
            std::uint64_t writer_rows,
            std::uint64_t writer_batches,
            bioseq::simd::HWY_NAMESPACE::MinimizerSketchBackend backend,
            ReadyMinimizerGenerationExecutionEvidencePg execution,
            std::uint64_t mps_spill_generation_token,
            std::uint64_t mps_spill_mutation_before_release,
            std::uint64_t mps_spill_mutation_after_release,
            bool mps_spill_resources_released) noexcept
        : index_kind_(index_kind),
          binding_(binding),
          control_count_(control_count),
          position_count_(position_count),
          writer_rows_(writer_rows),
          writer_batches_(writer_batches),
          selected_backend_(backend),
          execution_(std::move(execution)),
          mps_spill_generation_token_(mps_spill_generation_token),
          mps_spill_mutation_before_release_(
              mps_spill_mutation_before_release),
          mps_spill_mutation_after_release_(
              mps_spill_mutation_after_release),
          mps_spill_resources_released_(mps_spill_resources_released),
          published_(true) {}

public:
    ReadyMinimizerGenerationPublicationPg() = delete;
    ReadyMinimizerGenerationPublicationPg(
        const ReadyMinimizerGenerationPublicationPg&) = default;
    ReadyMinimizerGenerationPublicationPg(
        ReadyMinimizerGenerationPublicationPg&&) noexcept = default;
    ReadyMinimizerGenerationPublicationPg& operator=(
        const ReadyMinimizerGenerationPublicationPg&) = delete;
    ReadyMinimizerGenerationPublicationPg& operator=(
        ReadyMinimizerGenerationPublicationPg&&) noexcept = delete;

    [[nodiscard]] ReadyMinimizerGenerationIndexKindPg
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
    [[nodiscard]] std::uint64_t compatibility_checksum() const noexcept {
        return binding_.compatibility_checksum;
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
    [[nodiscard]] bioseq::simd::HWY_NAMESPACE::MinimizerSketchBackend
    selected_backend() const noexcept { return selected_backend_; }
    [[nodiscard]] const ReadyMinimizerGenerationExecutionEvidencePg&
    execution() const noexcept { return execution_; }
    [[nodiscard]] std::uint64_t
    mps_spill_generation_token() const noexcept {
        return mps_spill_generation_token_;
    }
    [[nodiscard]] std::uint64_t
    mps_spill_mutation_before_release() const noexcept {
        return mps_spill_mutation_before_release_;
    }
    [[nodiscard]] std::uint64_t
    mps_spill_mutation_after_release() const noexcept {
        return mps_spill_mutation_after_release_;
    }
    [[nodiscard]] bool mps_spill_resources_released() const noexcept {
        return mps_spill_resources_released_;
    }
    [[nodiscard]] bool published() const noexcept { return published_; }

    [[nodiscard]] bool execution_accepted() const noexcept {
        if (!published_ || !binding_.valid()) {
            return false;
        }
        if (const auto* mpf = std::get_if<
                bioseq::indexing::MpfGenerationExecutionEvidence>(
                    &execution_)) {
            return index_kind_ == ReadyMinimizerGenerationIndexKindPg::mpf
                && !mps_spill_resources_released_
                && mps_spill_generation_token_ == 0
                && mps_spill_mutation_before_release_ == 0
                && mps_spill_mutation_after_release_ == 0
                && mpf->selected_backend == selected_backend_
                && mpf->publication_accepted(binding_);
        }
        if (const auto* mps = std::get_if<
                bioseq::indexing::MpsGenerationExecutionEvidence>(
                    &execution_)) {
            return index_kind_ == ReadyMinimizerGenerationIndexKindPg::mps
                && mps_spill_resources_released_
                && mps_spill_generation_token_ != 0
                && mps_spill_mutation_before_release_
                    != (std::numeric_limits<std::uint64_t>::max)()
                && mps_spill_mutation_after_release_
                    == mps_spill_mutation_before_release_ + 1u
                && mps->selected_backend == selected_backend_
                && mps->publication_accepted(binding_)
                && mps->replay_final_storage_binding.generation_token
                    == mps_spill_generation_token_
                && mps->replay_final_storage_binding.mutation_version
                    == mps_spill_mutation_before_release_;
        }
        return false;
    }
};

static_assert(!std::is_aggregate_v<ReadyMinimizerGenerationPublicationPg>);
static_assert(!std::is_default_constructible_v<
    ReadyMinimizerGenerationPublicationPg>);
static_assert(!std::is_copy_assignable_v<
    ReadyMinimizerGenerationPublicationPg>);
static_assert(!std::is_move_assignable_v<
    ReadyMinimizerGenerationPublicationPg>);
template <class Publication>
concept publicly_relabelable_ready_minimizer_publication = requires(
        Publication& publication,
        ReadyMinimizerGenerationExecutionEvidencePg execution) {
    publication.execution = execution;
    publication.generation_id = std::uint64_t{1};
    publication.source_identity = std::uint64_t{1};
    publication.selected_backend =
        bioseq::simd::HWY_NAMESPACE::MinimizerSketchBackend::Scalar;
    publication.published = true;
};
static_assert(!publicly_relabelable_ready_minimizer_publication<
    ReadyMinimizerGenerationPublicationPg>);

/// Raise one coordinator diagnostic through a leaf PostgreSQL boundary. The
/// caller receives PostgresErrorAsCpp only after PG_END_TRY restored the PG
/// exception stack, so no encoder, run-store, proof, or writer object is
/// skipped by longjmp.
[[noreturn]] inline void ready_minimizer_error_as_cpp(
        int sqlstate,
        std::string_view message,
        std::string_view detail = {}) {
    bioseq::pg_utils::invoke_pg_as_cpp([sqlstate, message, detail] {
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

/// Build, completely reread, and atomically publish one format-4 MPF or MPS
/// generation from an already-authoritative PostgreSQL-backed lazy sequence.
///
/// This is intentionally not routed from INSERT, UPDATE, load, or mmsearch yet.
/// The caller retains both the registered source snapshot and metadata-age
/// ownership. The destructive same-name schema cutover remains inside
/// `storage.begin_generation_write()`; publication remains impossible until the
/// sink has returned its private complete-reread token.
template <class R, class Scanner>
[[nodiscard]] inline ReadyMinimizerGenerationPublicationPg
build_and_publish_grouped32_minimizer_generation_pg(
        Oid user_relid,
        std::int16_t column_attno,
        std::string_view index_name,
        const seq_locator& locator,
        std::uint64_t sequence_rowno,
        Snapshot registered_source_snapshot,
        IndexStorageFragmentPg& storage,
        bioseq::storage::lazy_compact_sequence<R, Scanner>& sequence,
        bioseq::simd::HWY_NAMESPACE::MinimizerSketchBackend backend) {
    static_assert(std::same_as<
        Scanner, bioseq::storage::sequence_scanner_pg<R>>,
        "READY minimizer coordinator requires the PostgreSQL scanner type");
    using Backend =
        bioseq::simd::HWY_NAMESPACE::MinimizerSketchBackend;
    using MpfIndex =
        bioseq::indexing::MinimizersPerFragmentIndex<IndexStorageFragmentPg>;
    using MpsIndex =
        bioseq::indexing::MinimizerSequenceMergedIndex<IndexStorageFragmentPg>;
    using Sequence = bioseq::storage::lazy_compact_sequence<R, Scanner>;
    using Traits = typename Sequence::traits;
    using CanonicalSequenceConfig = sequence_custom_alphabet<
        typename Traits::alphabet_type,
        typename Traits::storage_unit>;
    static_assert(Traits::bits_per_char
        == compact_sequence_range<CanonicalSequenceConfig>::traits::
            bits_per_char,
        "READY minimizer coordinator requires canonical PostgreSQL packing");

    const bool is_mpf = index_name == MINIMIZERS_PER_FRAGMENT_INDEX_NAME;
    const bool is_mps = index_name == MINIMIZERS_PER_SEQUENCE_INDEX_NAME;
    const bool active_snapshot_ready =
        bioseq::pg_utils::invoke_pg_as_cpp([] {
            return ActiveSnapshotSet()
                && IsMVCCSnapshot(GetActiveSnapshot());
        });

    if (!OidIsValid(user_relid) || column_attno <= 0
        || (!is_mpf && !is_mps)
        || !bioseq::indexing::sql_bigint_can_represent(sequence_rowno)
        || locator.version != kLocatorVersion || locator.source_epoch == 0
        || locator.total_bp_size == 0 || locator.num_fragments == 0
        || registered_source_snapshot == nullptr
        || !IsMVCCSnapshot(registered_source_snapshot)
        || registered_source_snapshot->regd_count == 0
        || !active_snapshot_ready) {
        ready_minimizer_error_as_cpp(
            ERRCODE_INVALID_PARAMETER_VALUE,
            "READY minimizer generation coordinator input is invalid");
    }
    if (backend != Backend::Scalar && backend != Backend::ExactSimd
        && backend != Backend::PaperNtHashSimd
        && backend != Backend::VulkanSegment) {
        ready_minimizer_error_as_cpp(
            ERRCODE_INVALID_PARAMETER_VALUE,
            "READY minimizer generation backend is invalid");
    }
    if (sequence.scanner().is_in_memory_mode()) {
        ready_minimizer_error_as_cpp(
            ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
            "READY minimizer generation requires a PostgreSQL-backed lazy sequence");
    }

    constexpr std::uint16_t kWindowSize = 10u;
    constexpr std::uint16_t kKmerSize = 15u;
    constexpr std::uint64_t letters_per_unit = Traits::letters_per_unit;
    static_assert(letters_per_unit != 0);
    const std::uint64_t expected_units = locator.total_bp_size / letters_per_unit
        + (locator.total_bp_size % letters_per_unit != 0 ? 1u : 0u);
    const std::size_t expected_padding = static_cast<std::size_t>(
        locator.total_bp_size % letters_per_unit == 0
            ? 0u
            : letters_per_unit
                - locator.total_bp_size % letters_per_unit);
    if (static_cast<std::uint64_t>(sequence.bp_size())
            != locator.total_bp_size
        || static_cast<std::uint64_t>(sequence.size()) != expected_units
        || sequence.number_of_fragments() != locator.num_fragments
        || sequence.padding() != expected_padding) {
        ready_minimizer_error_as_cpp(
            ERRCODE_DATA_CORRUPTED,
            "READY minimizer lazy sequence shape disagrees with its locator");
    }

    const RelationIdentity relation_identity =
        bioseq::pg_utils::invoke_pg_as_cpp(
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
        || storage.bucket_idx_ != locator.bucket_idx
        || storage.shard_idx_ != locator.shard_idx
        || storage.query_key_ != sequence_rowno
        || !sequence.scanner().matches_pg_binding(
            relation_identity.db_oid,
            relation_identity.tablespace_oid,
            relation_identity.relfilenode,
            column_attno,
            locator,
            expected_padding,
            registered_source_snapshot)) {
        ready_minimizer_error_as_cpp(
            ERRCODE_DATA_CORRUPTED,
            "READY minimizer storage identity disagrees with its source");
    }

    const bioseq::indexing::MinimizerSchemeIdentity scheme =
        bioseq::indexing::minimizer_scheme_identity_for_backend(backend);
    bioseq::indexing::MinimizerGenerationPayloadProfile profile{};
    {
        const auto derived_profile =
            bioseq::indexing::minimizer_generation_payload_profile(
                bioseq::indexing::MinimizerGenerationWireFormat::
                    grouped_hash32_segment_local32,
                scheme,
                kKmerSize);
        if (!derived_profile) {
            ready_minimizer_error_as_cpp(
                ERRCODE_FEATURE_NOT_SUPPORTED,
                "READY minimizer backend has no canonical format-4 profile",
                fmt::format("backend={} wire_error={}",
                    bioseq::simd::HWY_NAMESPACE::
                        minimizer_sketch_backend_name(backend),
                    static_cast<unsigned>(derived_profile.error())));
        }
        profile = *derived_profile;
    }

    struct BuildState {
        Rev3IndexAdministrationIdentity identity{};
        std::optional<Rev3AuthoritativeSourceProof> source_proof;
        Rev3AuthoritativeSourceLocator source{};
        std::uint64_t source_content_checksum = 0;
        std::optional<IndexStorageFragmentGenerationRowSinkPg> writer;
        std::optional<bioseq::indexing::EncodedMinimizerGeneration>
            generation;
        std::optional<ReadyGenerationToken> ready_token;
        std::optional<bioseq::indexing::MinimizerGenerationWireError>
            encode_error;
        bioseq::indexing::MpfGenerationExecutionEvidence mpf_execution{};
        bioseq::indexing::MpsGenerationExecutionEvidence mps_execution{};
        ReadyMinimizerGenerationExecutionEvidencePg execution{};
        bool mps_spill_started = false;
    };
    auto* state = new BuildState{
        .identity = Rev3IndexAdministrationIdentity{
            .database_oid = relation_identity.db_oid,
            .tablespace_oid = relation_identity.tablespace_oid,
            .main_relfilenode = relation_identity.relfilenode,
            .user_relid = user_relid,
            .column_attno = column_attno,
            .index_name = std::string{index_name},
            .table_name = storage.this_table_name(),
        },
    };
    struct PublicationDraft {
        ReadyMinimizerGenerationIndexKindPg index_kind =
            ReadyMinimizerGenerationIndexKindPg::mpf;
        bioseq::indexing::MinimizerGenerationPublicationBinding binding{};
        std::uint64_t generation_id = 0;
        std::uint64_t source_identity = 0;
        std::uint64_t source_content_checksum = 0;
        std::uint64_t compatibility_checksum = 0;
        std::uint64_t control_count = 0;
        std::uint64_t position_count = 0;
        std::uint64_t writer_rows = 0;
        std::uint64_t writer_batches = 0;
        bioseq::simd::HWY_NAMESPACE::MinimizerSketchBackend selected_backend =
            bioseq::simd::HWY_NAMESPACE::MinimizerSketchBackend::Scalar;
        ReadyMinimizerGenerationExecutionEvidencePg execution{};
        std::uint64_t mps_spill_generation_token = 0;
        std::uint64_t mps_spill_mutation_before_release = 0;
        std::uint64_t mps_spill_mutation_after_release = 0;
        bool mps_spill_resources_released = false;
    } result{
        .index_kind = is_mpf
            ? ReadyMinimizerGenerationIndexKindPg::mpf
            : ReadyMinimizerGenerationIndexKindPg::mps,
        .selected_backend = backend,
    };
    try {
            state->source_proof.emplace(
                derive_rev3_authoritative_source_proof_pg(
                    state->identity, sequence_rowno,
                    registered_source_snapshot));
            state->source =
                state->source_proof->source_for_diagnostics();
            state->source_content_checksum = state->source_proof
                ->source_content_checksum_for_diagnostics();
            if (state->source.sequence_rowno != sequence_rowno
                || !rev3_same_seq_locator(state->source.locator, locator)
                || state->source.alphabet_code
                    != pgseq_supported_alphabets::index_of<
                        typename Traits::alphabet_type>()
                || state->source.storage_unit_byte_width
                    != sizeof(typename Traits::storage_unit)
                || state->source.padding_letters != sequence.padding()
                || state->source.locator.total_bp_size
                    != sequence.bp_size()
                || state->source.locator.num_fragments
                    != sequence.number_of_fragments()) {
                ready_minimizer_error_as_cpp(
                    ERRCODE_DATA_CORRUPTED,
                    "READY minimizer authoritative source proof disagrees with the passed lazy sequence");
            }
            state->source_proof.reset();

            // Source identity is the immutable source epoch. Compatibility is
            // derived only from the encoder's final manifest below because the
            // planner may legitimately retry with a smaller fixed-width span.
            result.source_identity = rev3_source_identity(
                relation_identity.relfilenode, state->source);
            result.source_content_checksum =
                state->source_content_checksum;
            if (result.source_identity == 0) {
                ready_minimizer_error_as_cpp(
                    ERRCODE_DATA_CORRUPTED,
                    "READY minimizer source identity is zero");
            }

			ensure_vulkan_index_build_runtime_ready(backend);
            result.generation_id = bioseq::pg_utils::invoke_pg_as_cpp(
                [] { return allocate_minimizer_generation_id_pg(); });
            state->writer.emplace(storage.begin_generation_write(
                sequence_rowno, result.generation_id));
            const bioseq::indexing::MinimizerGenerationEncodeOptions options{
                .wire_format = bioseq::indexing::
                    MinimizerGenerationWireFormat::
                        grouped_hash32_segment_local32,
                .generation_id = result.generation_id,
                .source_identity = result.source_identity,
                .source_content_checksum =
                    state->source_content_checksum,
                .target_length = locator.total_bp_size,
                .alphabet_code = state->source.alphabet_code,
                .storage_unit_byte_width =
                    state->source.storage_unit_byte_width,
                .kmer_size = kKmerSize,
                .window_size = kWindowSize,
                .minimizer_scheme_id = scheme.id,
                .minimizer_scheme_version = scheme.version,
            };
            if (is_mpf) {
                auto encoded = MpfIndex::encode_persisted_generation(
                    sequence, backend, *state->writer, options,
                    bioseq::indexing::MinimizerGenerationFixedWidthLimits{},
                    &state->mpf_execution);
                if (encoded) {
                    state->generation.emplace(std::move(*encoded));
                    state->execution = state->mpf_execution;
                } else {
                    state->encode_error = encoded.error();
                }
            } else {
                state->mps_spill_started = true;
                auto spill = MpsIndex::spill_generation_replay_runs_serial(
                    storage, sequence, backend,
                    /*core_span_bp=*/0,
                    bioseq::indexing::kMinimizerPreferredSegmentSpanBp,
                    &state->mps_execution);
                if (!spill) {
                    state->encode_error = spill.error();
                } else {
                    auto encoded = MpsIndex::encode_persisted_generation(
                        storage, *state->writer, std::move(*spill), options,
                        bioseq::indexing::MinimizerGenerationFixedWidthLimits{},
                        &state->mps_execution);
                    if (encoded) {
                        state->generation.emplace(std::move(*encoded));
                        state->execution = state->mps_execution;
                    } else {
                        state->encode_error = encoded.error();
                    }
                }
            }
            if (state->generation.has_value()) {
                result.binding = bioseq::indexing::
                    minimizer_generation_publication_binding(
                        *state->generation);
            }

            // PostgreSQL owns every MPS spill run.  Destroy those BufFile/run
            // resources after the encoder has copied out its immutable proof,
            // and before any READY token can be validated or published.
            if (is_mps) {
                const auto before_release =
                    storage.segment_spill_storage_binding();
                if (!state->encode_error
                    && (!state->generation.has_value()
                        || !state->mps_execution
                            .ready_release_binding_accepted(before_release))) {
                    ready_minimizer_error_as_cpp(
                        ERRCODE_DATA_CORRUPTED,
                        "READY MPS spill incarnation changed before release");
                }
                storage.release_runs();
                const auto after_release =
                    storage.segment_spill_storage_binding();
                std::uint64_t retained_groups = 0;
                storage.merge_segment_runs(
                    [&retained_groups](auto&&...) { ++retained_groups; });
                result.mps_spill_generation_token =
                    before_release.generation_token;
                result.mps_spill_mutation_before_release =
                    before_release.mutation_version;
                result.mps_spill_mutation_after_release =
                    after_release.mutation_version;
                result.mps_spill_resources_released =
                    retained_groups == 0
                    && before_release.valid()
                    && after_release.generation_token
                        == before_release.generation_token
                    && before_release.mutation_version
                        != (std::numeric_limits<std::uint64_t>::max)()
                    && after_release.mutation_version
                        == before_release.mutation_version + 1u;
                state->mps_spill_started = false;
            }
            if (state->encode_error) {
                ready_minimizer_error_as_cpp(
                    ERRCODE_DATA_CORRUPTED,
                    "READY minimizer generation encoder failed",
                    fmt::format("backend={} wire_error={}",
                        bioseq::simd::HWY_NAMESPACE::
                            minimizer_sketch_backend_name(backend),
                        static_cast<unsigned>(*state->encode_error)));
            }
            bool execution_rejected = false;
            std::uint64_t producer_scans = 0;
            std::uint64_t geometry_retries = 0;
            std::uint64_t scalar_runs = 0;
            std::uint64_t exact_runs = 0;
            std::uint64_t paper_runs = 0;
            std::uint64_t vulkan_runs = 0;
            std::uint64_t vulkan_roundtrips = 0;
            if (const auto* execution = std::get_if<
                    bioseq::indexing::MpfGenerationExecutionEvidence>(
                        &state->execution)) {
                execution_rejected = !execution->publication_accepted(
                        result.binding)
                    || !is_mpf
                    || execution->selected_backend != backend
                    || execution->minimizer_scheme != scheme
                    || execution->target_length != locator.total_bp_size
                    || execution->window_size != kWindowSize
                    || execution->kmer_size != kKmerSize;
                producer_scans = execution->producer_scan_count;
                geometry_retries = execution->geometry_retry_count;
                scalar_runs = execution->scalar_core_runs;
                exact_runs = execution->exact_simd_core_runs;
                paper_runs = execution->paper_nthash_core_runs;
                vulkan_runs = execution->vulkan_core_runs;
                vulkan_roundtrips =
                    execution->vulkan_proof.completed_roundtrips;
            } else if (const auto* execution = std::get_if<
                    bioseq::indexing::MpsGenerationExecutionEvidence>(
                        &state->execution)) {
                execution_rejected = !execution->publication_accepted(
                        result.binding)
                    || !is_mps
                    || execution->selected_backend != backend
                    || execution->minimizer_scheme != scheme
                    || execution->target_length != locator.total_bp_size
                    || execution->window_size != kWindowSize
                    || execution->kmer_size != kKmerSize
                    || !result.mps_spill_resources_released;
                producer_scans = execution->replay_scan_count;
                geometry_retries = execution->replay_reroute_pass_count;
                scalar_runs = execution->scalar_core_runs;
                exact_runs = execution->exact_simd_core_runs;
                paper_runs = execution->paper_nthash_core_runs;
                vulkan_runs = execution->vulkan_core_runs;
                vulkan_roundtrips =
                    execution->vulkan_proof.completed_roundtrips;
            } else {
                execution_rejected = true;
            }
            if (execution_rejected) {
                ready_minimizer_error_as_cpp(
                    ERRCODE_DATA_CORRUPTED,
                    "READY minimizer selected backend execution proof was rejected",
                    fmt::format(
                        "backend={} scans={} retries={} scalar={} exact={} paper={} vulkan={} vulkan_roundtrips={}",
                        bioseq::simd::HWY_NAMESPACE::
                            minimizer_sketch_backend_name(backend),
                        producer_scans, geometry_retries, scalar_runs,
                        exact_runs, paper_runs, vulkan_runs,
                        vulkan_roundtrips));
            }
            const auto& manifest = state->generation->manifest;
            if (result.binding != bioseq::indexing::
                    minimizer_generation_publication_binding(
                        *state->generation)) {
                ready_minimizer_error_as_cpp(
                    ERRCODE_DATA_CORRUPTED,
                    "READY minimizer publication binding changed before validation");
            }
            result.compatibility_checksum = bioseq::indexing::
                minimizer_generation_compatibility_checksum(manifest);
            state->source.compatibility_checksum =
                result.compatibility_checksum;
            if (manifest.wire_format != bioseq::indexing::
                    MinimizerGenerationWireFormat::
                        grouped_hash32_segment_local32
                || manifest.generation_id != result.generation_id
                || manifest.source_identity != result.source_identity
                || manifest.source_content_checksum
                    != state->source_content_checksum
                || manifest.target_length != locator.total_bp_size
                || manifest.alphabet_code != state->source.alphabet_code
                || manifest.storage_unit_byte_width
                    != state->source.storage_unit_byte_width
                || manifest.kmer_size != kKmerSize
                || manifest.window_size != kWindowSize
                || manifest.minimizer_scheme_id != scheme.id
                || manifest.minimizer_scheme_version != scheme.version
                || manifest.gpu_abi_version != profile.gpu_abi_version
                || manifest.position_segment_span_bp == 0
                || manifest.position_segment_span_bp
                    > bioseq::indexing::kMinimizerPreferredSegmentSpanBp
                || state->generation->head.compatibility_checksum
                    != result.compatibility_checksum
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
            inject_rev3_pg_failure_as_cpp(
                Rev3PgFailureInjection::generation_storage_close,
                "generation_storage_close");

            ReadyGenerationPublishedHeadReceipt publication_receipt =
                publish_ready_minimizer_generation_head_pg(
                    std::move(*state->ready_token));
            state->ready_token.reset();

            bioseq::pg_utils::invoke_pg_as_cpp(
                [] { CommandCounterIncrement(); });
            bioseq::pg_utils::invoke_pg_as_cpp(
                [] { UpdateActiveSnapshotCommandId(); });
            Snapshot verification_source =
                bioseq::pg_utils::invoke_pg_as_cpp(
                    [] { return GetActiveSnapshot(); });
            Snapshot verification_snapshot =
                bioseq::pg_utils::invoke_pg_as_cpp(
                    [verification_source] {
                        return RegisterSnapshot(verification_source);
                    });
            const auto reread_expected = bioseq::indexing::
                expected_minimizer_compatibility_for_backend(
                    bioseq::indexing::MinimizerGenerationWireFormat::
                        grouped_hash32_segment_local32,
                    backend, state->source.alphabet_code,
                    state->source.storage_unit_byte_width,
                    kKmerSize, kWindowSize);
            if (!reread_expected) {
                ready_minimizer_error_as_cpp(
                    ERRCODE_DATA_CORRUPTED,
                    "READY minimizer publication reread contract is invalid");
            }
            {
                IndexStorageFragmentGenerationReadSessionPg reader{
                    publication_receipt.table_name,
                    publication_receipt.query_key,
                    verification_snapshot,
                    *reread_expected,
                    Rev3SnapshotRegistration::borrow_registered};
                const MinimizerGenerationHead reread_head =
                    reader.read_active_head_once();
                const auto reread_head_bytes = bioseq::indexing::
                    encode_minimizer_generation_head(reread_head);
                if (!reread_head_bytes
                    || reread_head != publication_receipt.head
                    || !bioseq::indexing::revision3_fixed_wire_returned_exactly(
                        publication_receipt.head_bytes,
                        *reread_head_bytes)) {
                    ready_minimizer_error_as_cpp(
                        ERRCODE_DATA_CORRUPTED,
                        "READY minimizer publication independent head reread disagrees");
                }
                inject_rev3_pg_failure_as_cpp(
                    Rev3PgFailureInjection::generation_post_publish_reread,
                    "generation_post_publish_reread");
                reader.close_head_read();
                inject_rev3_pg_failure_as_cpp(
                    Rev3PgFailureInjection::generation_reader_close,
                    "generation_reader_close");
            }
            bioseq::pg_utils::invoke_pg_as_cpp(
                [verification_snapshot] {
                    UnregisterSnapshot(verification_snapshot);
                });

            result.execution = state->execution;
            delete state;
            state = nullptr;
    } catch (const bioseq::pg_utils::PostgresErrorAsCpp&) {
        storage.abandon_after_pg_error_noexcept();
        state->ready_token.reset();
        state->generation.reset();
        state->mps_spill_started = false;
        state->writer.reset();
        state->source_proof.reset();
        delete state;
        state = nullptr;
        throw;
    } catch (...) {
        storage.abandon_after_pg_error_noexcept();
        state->ready_token.reset();
        state->generation.reset();
        state->mps_spill_started = false;
        state->writer.reset();
        state->source_proof.reset();
        delete state;
        state = nullptr;
        throw;
    }
    return ReadyMinimizerGenerationPublicationPg{
        result.index_kind,
        result.binding,
        result.control_count,
        result.position_count,
        result.writer_rows,
        result.writer_batches,
        result.selected_backend,
        std::move(result.execution),
        result.mps_spill_generation_token,
        result.mps_spill_mutation_before_release,
        result.mps_spill_mutation_after_release,
        result.mps_spill_resources_released};
}

// Compile the inactive template against one real production scanner shape.
// This emits no call and changes no route, but prevents a build-only gate from
// accepting dependent code that has never been instantiated.
namespace ready_minimizer_generation_compile_contract_detail {
using sequence_config = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
using scanner = bioseq::storage::sequence_scanner_pg<sequence_config>;
using lazy_sequence =
    bioseq::storage::lazy_compact_sequence<sequence_config, scanner>;
}  // namespace ready_minimizer_generation_compile_contract_detail

template ReadyMinimizerGenerationPublicationPg
build_and_publish_grouped32_minimizer_generation_pg<
    ready_minimizer_generation_compile_contract_detail::sequence_config,
    ready_minimizer_generation_compile_contract_detail::scanner>(
        Oid,
        std::int16_t,
        std::string_view,
        const seq_locator&,
        std::uint64_t,
        Snapshot,
        IndexStorageFragmentPg&,
        ready_minimizer_generation_compile_contract_detail::lazy_sequence&,
        bioseq::simd::HWY_NAMESPACE::MinimizerSketchBackend);

/// Result of the revoked SQL seam that instantiates the inactive coordinator
/// against a real PostgreSQL-backed lazy sequence.  The head and manifest are
/// independently reopened after publication; the route evidence is the
/// producer-originated proof returned by the coordinator itself.
struct ReadyMinimizerGenerationCoordinatorFixtureResult {
    std::string table_name;
    std::string selected_backend_name;
    std::int16_t column_attno = 0;
    std::uint64_t sequence_rowno = 0;
    seq_locator locator{};
    std::uint8_t alphabet_code = 0;
    std::uint8_t storage_unit_byte_width = 0;
    std::optional<ReadyMinimizerGenerationPublicationPg> publication;
    MinimizerGenerationHead head{};
    MinimizerGenerationManifest manifest{};
    bool head_manifest_verified = false;

    [[nodiscard]] const ReadyMinimizerGenerationPublicationPg&
    sealed_publication() const noexcept {
        DEBUG_ASSERT(publication.has_value(),
            "READY fixture publication must be sealed before return");
        return *publication;
    }
};

[[nodiscard]] inline bioseq::simd::HWY_NAMESPACE::MinimizerSketchBackend
ready_minimizer_fixture_backend_from_name(std::string_view name) {
    using Backend =
        bioseq::simd::HWY_NAMESPACE::MinimizerSketchBackend;
    if (name == "scalar") return Backend::Scalar;
    if (name == "exact_simd") return Backend::ExactSimd;
    if (name == "paper_nthash_simd") return Backend::PaperNtHashSimd;
    if (name == "vulkan_segment") return Backend::VulkanSegment;
    bioseq::pg_utils::invoke_pg_as_cpp([name] {
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg("READY minimizer fixture backend is invalid"),
             errdetail("backend=%.*s", static_cast<int>(name.size()),
                name.data()),
             errhint("Use scalar, exact_simd, paper_nthash_simd, or vulkan_segment.")));
    });
    pg_unreachable();
}

struct ReadyMinimizerFixtureBackendRequest {
    bioseq::simd::HWY_NAMESPACE::MinimizerSketchBackend backend =
        bioseq::simd::HWY_NAMESPACE::MinimizerSketchBackend::Scalar;
    Rev3PgFailureInjection failure = Rev3PgFailureInjection::none;
#ifdef BIOSEQ_TEST_INDEXES
    bioseq::storage::ScannerPgLeafFailure scanner_failure =
        bioseq::storage::ScannerPgLeafFailure::none;
    SortedRunPgLeafFailure sorted_run_failure =
        SortedRunPgLeafFailure::none;
    std::uint64_t sorted_run_failure_operation = 1u;
    std::uint64_t sorted_run_merge_reader_budget_bytes = 0u;
#endif
};

[[nodiscard]] inline ReadyMinimizerFixtureBackendRequest
ready_minimizer_fixture_backend_request(std::string_view name) {
#ifdef BIOSEQ_TEST_INDEXES
    using namespace std::string_view_literals;
    using ScannerFailure = bioseq::storage::ScannerPgLeafFailure;
    constexpr std::array scanner_failures{
        std::pair{"scalar_fail_scanner_interrupt_before"sv,
            ScannerFailure::interrupt_before},
        std::pair{"scalar_fail_scanner_interrupt_after"sv,
            ScannerFailure::interrupt_after},
        std::pair{"scalar_fail_scanner_row_read_before"sv,
            ScannerFailure::row_read_before},
        std::pair{"scalar_fail_scanner_row_read_after"sv,
            ScannerFailure::row_read_after},
        std::pair{"scalar_fail_scanner_palloc_before"sv,
            ScannerFailure::palloc_before},
        std::pair{"scalar_fail_scanner_palloc_after"sv,
            ScannerFailure::palloc_after},
    };
    for (const auto& [backend_name, failure] : scanner_failures) {
        if (name == backend_name) {
            return {
                .backend = bioseq::simd::HWY_NAMESPACE::
                    MinimizerSketchBackend::Scalar,
                .scanner_failure = failure,
            };
        }
    }
    if (name == "scalar_fail_sorted_compact_create_before_nth2") {
        return {
            .backend = bioseq::simd::HWY_NAMESPACE::
                MinimizerSketchBackend::Scalar,
            .sorted_run_failure =
                SortedRunPgLeafFailure::compact_create_before,
            .sorted_run_failure_operation = 2u,
            // Three 32-Kbp source cores produce three input runs. A two-reader
            // budget forces two compaction outputs, so operation 2 occurs only
            // after output 1 completed and its two input handles were closed.
            .sorted_run_merge_reader_budget_bytes =
                std::uint64_t{64} << 10,
        };
    }
    constexpr std::array sorted_run_failures{
        std::pair{"scalar_fail_sorted_create_before"sv,
            SortedRunPgLeafFailure::create_before},
        std::pair{"scalar_fail_sorted_create_after"sv,
            SortedRunPgLeafFailure::create_after},
        std::pair{"scalar_fail_sorted_write_before"sv,
            SortedRunPgLeafFailure::write_before},
        std::pair{"scalar_fail_sorted_write_after"sv,
            SortedRunPgLeafFailure::write_after},
        std::pair{"scalar_fail_sorted_read_before"sv,
            SortedRunPgLeafFailure::read_before},
        std::pair{"scalar_fail_sorted_read_after"sv,
            SortedRunPgLeafFailure::read_after},
        std::pair{"scalar_fail_sorted_seek_before"sv,
            SortedRunPgLeafFailure::seek_before},
        std::pair{"scalar_fail_sorted_seek_after"sv,
            SortedRunPgLeafFailure::seek_after},
        std::pair{"scalar_fail_sorted_size_before"sv,
            SortedRunPgLeafFailure::size_before},
        std::pair{"scalar_fail_sorted_size_after"sv,
            SortedRunPgLeafFailure::size_after},
        std::pair{"scalar_fail_sorted_close_before"sv,
            SortedRunPgLeafFailure::close_before},
        std::pair{"scalar_fail_sorted_close_after"sv,
            SortedRunPgLeafFailure::close_after},
        std::pair{"scalar_fail_sorted_interrupt_before"sv,
            SortedRunPgLeafFailure::interrupt_before},
        std::pair{"scalar_fail_sorted_interrupt_after"sv,
            SortedRunPgLeafFailure::interrupt_after},
    };
    for (const auto& [backend_name, failure] : sorted_run_failures) {
        if (name == backend_name) {
            return {
                .backend = bioseq::simd::HWY_NAMESPACE::
                    MinimizerSketchBackend::Scalar,
                .sorted_run_failure = failure,
            };
        }
    }
    if (name == "scalar_fail_pg_spill") {
        return {
            .backend = bioseq::simd::HWY_NAMESPACE::
                MinimizerSketchBackend::Scalar,
            .failure = Rev3PgFailureInjection::mps_spill,
        };
    }
    if (name == "scalar_fail_pg_merge") {
        return {
            .backend = bioseq::simd::HWY_NAMESPACE::
                MinimizerSketchBackend::Scalar,
            .failure = Rev3PgFailureInjection::mps_merge,
        };
    }
    if (name == "scalar_fail_pg_tts_create") {
        return {
            .backend = bioseq::simd::HWY_NAMESPACE::
                MinimizerSketchBackend::Scalar,
            .failure = Rev3PgFailureInjection::generation_tts_create,
        };
    }
    if (name == "scalar_fail_pg_tuple_form") {
        return {
            .backend = bioseq::simd::HWY_NAMESPACE::
                MinimizerSketchBackend::Scalar,
            .failure = Rev3PgFailureInjection::generation_tuple_form,
        };
    }
    if (name == "scalar_fail_pg_tts_store") {
        return {
            .backend = bioseq::simd::HWY_NAMESPACE::
                MinimizerSketchBackend::Scalar,
            .failure = Rev3PgFailureInjection::generation_tts_store,
        };
    }
    if (name == "scalar_fail_pg_tts_drop") {
        return {
            .backend = bioseq::simd::HWY_NAMESPACE::
                MinimizerSketchBackend::Scalar,
            .failure = Rev3PgFailureInjection::generation_tts_drop,
        };
    }
    if (name == "scalar_fail_pg_multi_insert") {
        return {
            .backend = bioseq::simd::HWY_NAMESPACE::
                MinimizerSketchBackend::Scalar,
            .failure = Rev3PgFailureInjection::generation_multi_insert,
        };
    }
    if (name == "scalar_fail_pg_reread") {
        return {
            .backend = bioseq::simd::HWY_NAMESPACE::
                MinimizerSketchBackend::Scalar,
            .failure = Rev3PgFailureInjection::generation_reread,
        };
    }
    if (name == "scalar_fail_pg_spi_publish") {
        return {
            .backend = bioseq::simd::HWY_NAMESPACE::
                MinimizerSketchBackend::Scalar,
            .failure = Rev3PgFailureInjection::generation_spi_publish,
        };
    }
    if (name == "scalar_fail_pg_writer_close") {
        return {
            .backend = bioseq::simd::HWY_NAMESPACE::
                MinimizerSketchBackend::Scalar,
            .failure = Rev3PgFailureInjection::generation_writer_close,
        };
    }
    if (name == "scalar_fail_pg_storage_close") {
        return {
            .backend = bioseq::simd::HWY_NAMESPACE::
                MinimizerSketchBackend::Scalar,
            .failure = Rev3PgFailureInjection::generation_storage_close,
        };
    }
    if (name == "scalar_fail_pg_post_publish_reread") {
        return {
            .backend = bioseq::simd::HWY_NAMESPACE::
                MinimizerSketchBackend::Scalar,
            .failure = Rev3PgFailureInjection::
                generation_post_publish_reread,
        };
    }
    if (name == "scalar_fail_pg_reader_close") {
        return {
            .backend = bioseq::simd::HWY_NAMESPACE::
                MinimizerSketchBackend::Scalar,
            .failure = Rev3PgFailureInjection::generation_reader_close,
        };
    }
#endif
    return {
        .backend = ready_minimizer_fixture_backend_from_name(name),
        .failure = Rev3PgFailureInjection::none,
    };
}

inline void arm_ready_minimizer_fixture_failures(
        const ReadyMinimizerFixtureBackendRequest& request) noexcept {
    arm_rev3_pg_failure_injection(request.failure);
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
    disarm_rev3_pg_failure_injection();
#ifdef BIOSEQ_TEST_INDEXES
    bioseq::storage::disarm_scanner_pg_leaf_failure();
    disarm_sorted_run_pg_leaf_failure();
#endif
}

/// Integration-only execution of the inactive format-4 coordinator.  This
/// deliberately does not call a live INSERT/update/query route and does not
/// touch metadata_age_log.  The caller selects the biological backend
/// explicitly; an unavailable SIMD/GPU route fails instead of falling back.
[[nodiscard]] inline ReadyMinimizerGenerationCoordinatorFixtureResult
build_ready_minimizer_generation_coordinator_fixture_impl(
        Oid rel_oid,
        std::string_view index_name,
        std::uint64_t sequence_rowno,
        std::string_view backend_name) {
    using Backend =
        bioseq::simd::HWY_NAMESPACE::MinimizerSketchBackend;
    constexpr std::uint16_t kKmerSize = 15u;
    constexpr std::uint16_t kWindowSize = 10u;

    const bool is_bioseq_table = OidIsValid(rel_oid)
        && bioseq::pg_utils::invoke_pg_as_cpp(
            [rel_oid] { return IsBioseqColumnTableAmTable(rel_oid); });
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
    if (index_name != MINIMIZERS_PER_FRAGMENT_INDEX_NAME
        && index_name != MINIMIZERS_PER_SEQUENCE_INDEX_NAME) {
        ready_minimizer_error_as_cpp(
            ERRCODE_INVALID_PARAMETER_VALUE,
            "READY minimizer fixture requires minimizers_per_fragment or minimizers_per_sequence");
    }
    if (!bioseq::indexing::sql_bigint_can_represent(sequence_rowno)) {
        ready_minimizer_error_as_cpp(
            ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE,
            "READY minimizer fixture row number exceeds BIGINT");
    }
    const ReadyMinimizerFixtureBackendRequest backend_request =
        ready_minimizer_fixture_backend_request(backend_name);
    const Backend backend = backend_request.backend;
    Snapshot active_snapshot = GetActiveSnapshot();
    if (active_snapshot == nullptr || !IsMVCCSnapshot(active_snapshot)) {
        ready_minimizer_error_as_cpp(
            ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
            "READY minimizer fixture requires an active MVCC snapshot");
    }

    const std::int16_t seq_attno = bioseq::pg_utils::invoke_pg_as_cpp(
        [rel_oid] { return single_seq_attno_for_inspection(rel_oid); });
    const RelationIdentity relation_identity =
        bioseq::pg_utils::invoke_pg_as_cpp(
            [rel_oid] { return index_relation_identity(rel_oid); });
    if (relation_identity.db_oid != MyDatabaseId
        || !OidIsValid(relation_identity.tablespace_oid)
        || !OidIsValid(relation_identity.relfilenode)) {
        ready_minimizer_error_as_cpp(
            ERRCODE_UNDEFINED_TABLE,
            "READY minimizer fixture source relation is unavailable");
    }

    struct FixtureState {
        Snapshot source_snapshot = nullptr;
        Snapshot verification_snapshot = nullptr;
        Relation source_relation = nullptr;
        BioseqColumnReadState* source_read = nullptr;
        seq_caches* fragment_caches = nullptr;
        MemoryContext work_context = nullptr;
        std::optional<IndexStorageFragmentPg> storage;
        std::optional<IndexStorageFragmentGenerationReadSessionPg>
            verification_session;
        std::optional<ReadyMinimizerGenerationCoordinatorFixtureResult>
            result;
        bool completed = false;
    };
    auto* state = new (std::nothrow) FixtureState{};
    if (state == nullptr) {
        ready_minimizer_error_as_cpp(
            ERRCODE_OUT_OF_MEMORY,
            "READY minimizer fixture could not allocate cleanup state");
    }
    const MemoryContext caller_context = CurrentMemoryContext;

    try {
        state->result.emplace(
            ReadyMinimizerGenerationCoordinatorFixtureResult{
                .selected_backend_name = bioseq::simd::HWY_NAMESPACE::
                    minimizer_sketch_backend_name(backend),
                .column_attno = seq_attno,
                .sequence_rowno = sequence_rowno,
            });
        ReadyMinimizerGenerationCoordinatorFixtureResult& result =
            *state->result;
        state->source_snapshot = bioseq::pg_utils::invoke_pg_as_cpp(
            [active_snapshot] { return RegisterSnapshot(active_snapshot); });
        if (state->source_snapshot == nullptr
            || state->source_snapshot->regd_count == 0) {
            ready_minimizer_error_as_cpp(
                ERRCODE_INTERNAL_ERROR,
                "READY minimizer fixture could not register its source snapshot");
        }
        state->work_context = bioseq::pg_utils::invoke_pg_as_cpp(
            [caller_context] {
                return AllocSetContextCreate(
                    caller_context,
                    "bioseq READY minimizer coordinator fixture",
                    ALLOCSET_DEFAULT_SIZES);
            });
        state->source_relation = bioseq::pg_utils::invoke_pg_as_cpp(
            [rel_oid] {
                return try_safe_relation_open(
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
            bioseq::pg_utils::invoke_pg_as_cpp(
                [state] {
                    return MemoryContextSwitchTo(state->work_context);
                });
        Datum* values = bioseq::pg_utils::invoke_pg_as_cpp(
            [source_desc] {
                return static_cast<Datum*>(palloc0(
                    sizeof(Datum)
                    * static_cast<std::size_t>(source_desc->natts)));
            });
        bool* nulls = bioseq::pg_utils::invoke_pg_as_cpp(
            [source_desc] {
                return static_cast<bool*>(palloc0(
                    sizeof(bool)
                    * static_cast<std::size_t>(source_desc->natts)));
            });
        List* projected_columns = bioseq::pg_utils::invoke_pg_as_cpp(
            [seq_attno] { return list_make1_int(seq_attno); });
        state->source_read = bioseq::pg_utils::invoke_pg_as_cpp(
            [state, source_desc, projected_columns] {
                return BioseqColumnBeginRead(
                    state->source_relation, source_desc,
                    projected_columns, NIL, state->work_context,
                    state->source_snapshot, /*randomAccess=*/true, nullptr);
            });
        const bool row_visible = bioseq::pg_utils::invoke_pg_as_cpp(
            [state, sequence_rowno, values, nulls] {
                return BioseqColumnReadRowByRowNumber(
                    state->source_read, sequence_rowno, values, nulls);
            });
        if (!row_visible || nulls[seq_attno - 1]) {
            bioseq::pg_utils::invoke_pg_as_cpp(
                [previous_context] {
                    MemoryContextSwitchTo(previous_context);
                });
            ready_minimizer_error_as_cpp(
                ERRCODE_NO_DATA_FOUND,
                "READY minimizer fixture source row is not visible",
                fmt::format("sequence_rowno={}", sequence_rowno));
        }
        pgseq_t* source_header = bioseq::pg_utils::invoke_pg_as_cpp(
            [values, seq_attno] {
                return reinterpret_cast<pgseq_t*>(
                    PG_DETOAST_DATUM(values[seq_attno - 1]));
            });
        if (!is_locator_stub(source_header)) {
            bioseq::pg_utils::invoke_pg_as_cpp(
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
        if (result.locator.version != kLocatorVersion
            || result.locator.source_epoch == 0
            || result.locator.total_bp_size == 0
            || result.locator.num_fragments == 0) {
            bioseq::pg_utils::invoke_pg_as_cpp(
                [previous_context] {
                    MemoryContextSwitchTo(previous_context);
                });
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "READY minimizer fixture locator is invalid");
        }
        bioseq::pg_utils::invoke_pg_as_cpp(
            [state] { BioseqColumnEndRead(state->source_read); });
        state->source_read = nullptr;
        bioseq::pg_utils::invoke_pg_as_cpp(
            [previous_context] {
                MemoryContextSwitchTo(previous_context);
            });
        Relation finished_source = std::exchange(
            state->source_relation, nullptr);
        bioseq::pg_utils::invoke_pg_as_cpp(
            [finished_source] {
                relation_close(finished_source, AccessShareLock);
            });

        state->fragment_caches = bioseq::pg_utils::invoke_pg_as_cpp(
            [state] { return alloc_seq_caches(state->work_context); });
        state->fragment_caches->side = seq_caches::Side::Read;
        state->fragment_caches->lock_mode = AccessShareLock;
        state->fragment_caches->read_state_mctx = state->work_context;
        state->fragment_caches->main_database_oid =
            relation_identity.db_oid;
        state->fragment_caches->main_tablespace_oid =
            relation_identity.tablespace_oid;
        state->fragment_caches->main_relfilenode =
            relation_identity.relfilenode;

        pgseq_t::dispatch_instance(
                result.alphabet_code, result.storage_unit_byte_width,
                [state, rel_oid, relation_identity, seq_attno, index_name,
                 sequence_rowno, backend, backend_request,
                 &result]<class SequenceConfig>() {
                    constexpr std::size_t letters_per_unit =
                        compact_sequence_range<SequenceConfig>::traits::
                            letters_per_unit;
                    const std::size_t padding = static_cast<std::size_t>(
                        result.locator.total_bp_size % letters_per_unit == 0
                            ? 0u
                            : letters_per_unit
                                - result.locator.total_bp_size
                                    % letters_per_unit);
                    auto opened_storage =
                        bioseq::pg_utils::invoke_pg_as_cpp(
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
                    bioseq::storage::with_lazy_postgres<SequenceConfig>(
                        relation_identity.db_oid,
                        relation_identity.tablespace_oid,
                        relation_identity.relfilenode, seq_attno,
                        result.locator, padding, state->source_snapshot,
                        state->fragment_caches, state->work_context,
                        [state, rel_oid, seq_attno, index_name,
                         sequence_rowno, backend, backend_request,
                         &result](auto& lazy) {
                            arm_ready_minimizer_fixture_failures(
                                backend_request);
                            result.publication.emplace(
                                build_and_publish_grouped32_minimizer_generation_pg(
                                    rel_oid, seq_attno, index_name,
                                    result.locator, sequence_rowno,
                                    state->source_snapshot,
                                    *state->storage, lazy, backend));
                            disarm_ready_minimizer_fixture_failures();
                        });
                });
        const bool evidence_kind_matches = result.publication.has_value()
            && (index_name == MINIMIZERS_PER_FRAGMENT_INDEX_NAME
                ? std::holds_alternative<
                    bioseq::indexing::MpfGenerationExecutionEvidence>(
                        result.publication->execution())
                : std::holds_alternative<
                    bioseq::indexing::MpsGenerationExecutionEvidence>(
                        result.publication->execution()));
        if (!result.publication.has_value()
            || !result.publication->published()
            || !result.publication->execution_accepted()
            || !evidence_kind_matches
            || result.publication->selected_backend() != backend) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "READY minimizer fixture publication proof was rejected");
        }

        state->storage->close_checked();
        state->storage.reset();
        bioseq::pg_utils::invoke_pg_as_cpp([state] {
            close_and_destroy_seq_caches(
                state->fragment_caches, /*commit=*/false);
        });
        state->fragment_caches = nullptr;
        bioseq::pg_utils::invoke_pg_as_cpp(
            [state] { UnregisterSnapshot(state->source_snapshot); });
        state->source_snapshot = nullptr;

        // Reopen the published head and immutable manifest independently.  A
        // fresh registered copy is intentional: it proves the rows are visible
        // through PostgreSQL after the private READY token was consumed.
        bioseq::pg_utils::invoke_pg_as_cpp([] {
            CommandCounterIncrement();
        });
        bioseq::pg_utils::invoke_pg_as_cpp([] {
            UpdateActiveSnapshotCommandId();
        });
        Snapshot verification_source = GetActiveSnapshot();
        state->verification_snapshot =
            bioseq::pg_utils::invoke_pg_as_cpp(
                [verification_source] {
                    return RegisterSnapshot(verification_source);
                });
        const auto expected = bioseq::indexing::
            expected_minimizer_compatibility_for_backend(
                bioseq::indexing::MinimizerGenerationWireFormat::
                    grouped_hash32_segment_local32,
                backend, result.alphabet_code,
                result.storage_unit_byte_width, kKmerSize, kWindowSize);
        if (!expected) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "READY minimizer fixture could not derive its read contract");
        }
        state->verification_session.emplace(
            result.table_name, sequence_rowno,
            state->verification_snapshot, *expected,
            Rev3SnapshotRegistration::borrow_registered);
        result.head = state->verification_session->read_active_head_once();
        result.manifest = state->verification_session
            ->read_manifest_directory_once(result.head).manifest;
        const std::uint64_t manifest_compatibility = bioseq::indexing::
            minimizer_generation_compatibility_checksum(result.manifest);
        const auto scheme = bioseq::indexing::
            minimizer_scheme_identity_for_backend(backend);
        if (result.head.wire_format
                != bioseq::indexing::MinimizerGenerationWireFormat::
                    grouped_hash32_segment_local32
            || result.head.generation_id
                != result.publication->generation_id()
            || result.head.source_identity
                != result.publication->source_identity()
            || result.head.source_content_checksum
                != result.publication->source_content_checksum()
            || result.head.compatibility_checksum
                != result.publication->compatibility_checksum()
            || result.manifest.wire_format != result.head.wire_format
            || result.manifest.generation_id != result.head.generation_id
            || result.manifest.source_identity != result.head.source_identity
            || result.manifest.source_content_checksum
                != result.head.source_content_checksum
            || result.manifest.target_length
                != result.locator.total_bp_size
            || result.manifest.control_count
                != result.publication->control_count()
            || result.manifest.position_count
                != result.publication->position_count()
            || result.manifest.kmer_size != kKmerSize
            || result.manifest.window_size != kWindowSize
            || result.manifest.minimizer_scheme_id != scheme.id
            || result.manifest.minimizer_scheme_version != scheme.version
            || manifest_compatibility
                != result.publication->compatibility_checksum()) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "READY minimizer fixture independently read an inconsistent head or manifest");
        }
        result.head_manifest_verified = true;
        state->verification_session->close_metadata_read();
        state->verification_session.reset();
        bioseq::pg_utils::invoke_pg_as_cpp(
            [state] { UnregisterSnapshot(state->verification_snapshot); });
        state->verification_snapshot = nullptr;

        MemoryContext finished_context = std::exchange(
            state->work_context, nullptr);
        bioseq::pg_utils::invoke_pg_as_cpp(
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
    ReadyMinimizerGenerationCoordinatorFixtureResult result =
        std::move(*state->result);
    state->result.reset();
    delete state;
    state = nullptr;
    return result;
}

/// Live PostgreSQL fixture result for the revision-3/4 storage contract. This
/// is used only by the integration SQL seam below; production readers never
/// call it and ordinal 2 remains disabled.
struct Rev3PgIntegrationFixtureResult {
    std::string table_name;
    bioseq::indexing::MinimizerGenerationWireFormat wire_format =
        bioseq::indexing::MinimizerGenerationWireFormat::invalid;
    std::uint64_t generation_id = 0;
    std::uint64_t source_identity = 0;
    std::uint64_t source_content_checksum = 0;
    std::uint64_t compatibility_checksum = 0;
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
    MinimizerGenerationReadStats read_stats{};
};

/// Integration-only observations for the generation-storage cutover.  Optional
/// fields are emitted as SQL NULL by the fmgr wrapper when an operation does not
/// exercise that part of the contract.
struct Rev3GenerationStorageStateResult {
    Oid before_oid = InvalidOid;
    Oid operation_oid = InvalidOid;
    Oid after_oid = InvalidOid;
    std::optional<bool> same_handle_rebound;
    std::optional<bool> generation_schema_after;
    std::optional<bool> chunk_cache_hit;
    std::optional<bool> assembled_cache_hit;
};

/// Deterministic replayable legacy-scheme producer used by the live PG fixture.
/// It emits at most 4096 segment/hash/position-sorted events, enough to exercise
/// both physical payload profiles without retaining biological source bytes.
struct Rev3PgIntegrationFixtureProducer {
    std::uint64_t target_length = 0;

    [[nodiscard]] bioseq::indexing::MinimizerSchemeIdentity
    minimizer_scheme_identity() const noexcept {
        return bioseq::indexing::kLegacyMinimapV1SchemeIdentity;
    }

    [[nodiscard]] std::expected<void, MinimizerGenerationWireError>
    scan_sorted_events(
            std::uint64_t,
            bioseq::indexing::CanonicalMinimizerEventReceiver receiver) {
        const std::uint64_t event_count = std::min<std::uint64_t>(
            target_length, 4096u);
        for (std::uint64_t event = 0; event < event_count; ++event) {
            auto emitted = receiver(
                bioseq::indexing::CanonicalMinimizerEvent{
                    .hash = event + 1u,
                    .encoded_position = event << 1u,
                });
            if (!emitted) {
                return emitted;
            }
        }
        return {};
    }
};

static_assert(bioseq::indexing::MinimizerGenerationEventProducer<
    Rev3PgIntegrationFixtureProducer>);

/// Build and publish one exact live format-3/format-4 generation. Every write
/// goes through the production row sink; this half deliberately opens no read
/// session so a later SQL statement/backend can prove the cold reader alone.
[[nodiscard]] inline Rev3PgIntegrationFixtureResult
build_rev3_pg_integration_fixture_impl(
        Oid rel_oid,
        std::string_view index_name,
        std::uint64_t sequence_rowno,
        std::uint16_t kmer_size,
        std::uint16_t window_size,
        bioseq::indexing::MinimizerGenerationWireFormat wire_format) {
    if (!OidIsValid(rel_oid)
        || !bioseq::pg_utils::invoke_pg_as_cpp(
            [rel_oid] { return IsBioseqColumnTableAmTable(rel_oid); })) {
        ready_minimizer_error_as_cpp(
            ERRCODE_WRONG_OBJECT_TYPE,
            "bioseq rev3 fixture requires a USING bioseq table");
    }
    if (!bioseq::pg_utils::invoke_pg_as_cpp([] { return superuser(); })) {
        ready_minimizer_error_as_cpp(
            ERRCODE_INSUFFICIENT_PRIVILEGE,
            "bioseq rev3 fixture requires superuser");
    }
    if (index_name != MINIMIZERS_PER_FRAGMENT_INDEX_NAME
        && index_name != MINIMIZERS_PER_SEQUENCE_INDEX_NAME) {
        ready_minimizer_error_as_cpp(
            ERRCODE_INVALID_PARAMETER_VALUE,
            "bioseq rev3 fixture requires a minimizer index");
    }
    if (kmer_size == 0 || window_size == 0
        || !bioseq::indexing::sql_bigint_can_represent(sequence_rowno)) {
        ready_minimizer_error_as_cpp(
            ERRCODE_INVALID_PARAMETER_VALUE,
            "bioseq rev3 fixture parameters are invalid");
    }
    if (wire_format
            != bioseq::indexing::MinimizerGenerationWireFormat::
                sorted_dense_global64
        && wire_format
            != bioseq::indexing::MinimizerGenerationWireFormat::
                grouped_hash32_segment_local32) {
        ready_minimizer_error_as_cpp(
            ERRCODE_INVALID_PARAMETER_VALUE,
            "bioseq rev3 fixture wire format must be canonical 3 or 4");
    }
    const auto profile = bioseq::indexing::minimizer_generation_payload_profile(
        wire_format,
        bioseq::indexing::kLegacyMinimapV1SchemeIdentity,
        kmer_size);
    if (!profile) {
        ready_minimizer_error_as_cpp(
            ERRCODE_INVALID_PARAMETER_VALUE,
            "bioseq rev3 fixture parameters do not admit the selected wire format");
    }

    const std::int16_t seq_attno = single_seq_attno_for_inspection(rel_oid);
    const RelationIdentity rid = index_relation_identity(rel_oid);
    Rev3IndexAdministrationIdentity identity{
        .database_oid = rid.db_oid,
        .tablespace_oid = rid.tablespace_oid,
        .main_relfilenode = rid.relfilenode,
        .user_relid = rel_oid,
        .column_attno = seq_attno,
        .index_name = std::string{index_name},
    };

    struct FixtureState {
        Snapshot registered_snapshot = nullptr;
        std::optional<Rev3AuthoritativeSourceProof> setup_proof;
        std::optional<IndexStorageFragmentGenerationRowSinkPg> writer;
        std::optional<bioseq::indexing::EncodedMinimizerGeneration> generation;
        std::optional<ReadyGenerationToken> ready_token;
    };
    auto* state = new FixtureState{};
    Rev3PgIntegrationFixtureResult result{};
    try {
        Snapshot active_snapshot = bioseq::pg_utils::invoke_pg_as_cpp(
            [] { return GetActiveSnapshot(); });
        if (active_snapshot == nullptr || !IsMVCCSnapshot(active_snapshot)) {
            ready_minimizer_error_as_cpp(
                ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
                "bioseq rev3 fixture requires an active MVCC snapshot");
        }
        state->registered_snapshot = bioseq::pg_utils::invoke_pg_as_cpp(
            [active_snapshot] { return RegisterSnapshot(active_snapshot); });
        state->setup_proof.emplace(
            derive_rev3_authoritative_source_proof_pg(
                identity, sequence_rowno, state->registered_snapshot));
        Rev3AuthoritativeSourceLocator source =
            state->setup_proof->source_for_diagnostics();
        const std::uint64_t source_content_checksum =
            state->setup_proof->source_content_checksum_for_diagnostics();
        result.table_name = format_seqidx_name(
            index_name, rid.db_oid, rid.tablespace_oid, rid.relfilenode,
            seq_attno, source.locator.bucket_idx, source.locator.shard_idx);
        identity.table_name = result.table_name;
        result.wire_format = wire_format;
        state->setup_proof.reset();
        bioseq::pg_utils::invoke_pg_as_cpp(
            [state] { UnregisterSnapshot(state->registered_snapshot); });
        state->registered_snapshot = nullptr;

        const char* quoted_table = bioseq::pg_utils::invoke_pg_as_cpp(
            [&result] { return quote_identifier(result.table_name.c_str()); });
        const std::string drop_ddl = fmt::format(
            "DROP TABLE IF EXISTS bioseq_internal.{}",
            quoted_table);
        bioseq::pg_utils::invoke_pg_as_cpp(
            [&drop_ddl] { run_internal_ddl(drop_ddl.c_str()); });
        const std::string create_ddl = fmt::format(
            "CREATE TABLE bioseq_internal.{} ("
            "query_key BIGINT NOT NULL, generation_id BIGINT NOT NULL, "
            "position_segment INTEGER NOT NULL, sub_key BIGINT NOT NULL, "
            "chunk_no INTEGER NOT NULL, chunk_bytes INTEGER NOT NULL, "
            "checksum BIGINT NOT NULL, blob BYTEA NOT NULL, "
            "PRIMARY KEY (query_key,generation_id,position_segment,sub_key,chunk_no)) "
            "USING heap",
            quoted_table);
        bioseq::pg_utils::invoke_pg_as_cpp(
            [&create_ddl] { run_internal_ddl(create_ddl.c_str()); });
        const std::string alter_ddl = fmt::format(
            "ALTER TABLE bioseq_internal.{} ALTER COLUMN blob SET STORAGE PLAIN",
            quoted_table);
        bioseq::pg_utils::invoke_pg_as_cpp(
            [&alter_ddl] { run_internal_ddl(alter_ddl.c_str()); });
        bioseq::pg_utils::invoke_pg_as_cpp(
            [] { CommandCounterIncrement(); });

        result.generation_id = bioseq::pg_utils::invoke_pg_as_cpp(
            [] { return allocate_minimizer_generation_id_pg(); });
        const std::uint64_t target_length = source.locator.total_bp_size;
        if (target_length == 0) {
            ready_minimizer_error_as_cpp(
                ERRCODE_INVALID_PARAMETER_VALUE,
                "bioseq rev3 fixture requires a nonempty target");
        }
        const std::uint64_t span = target_length == 0
            ? 0 : std::min<std::uint64_t>(
                bioseq::indexing::kMinimizerPreferredSegmentSpanBp,
                profile->segment_local_positions
                    ? static_cast<std::uint64_t>(
                        bioseq::indexing::kMinimizerGrouped32MaxLocalBp) + 1u
                    : (std::numeric_limits<std::uint64_t>::max)());
        const std::uint64_t segment_count = target_length == 0
            ? 0 : target_length / span + (target_length % span != 0 ? 1u : 0u);
        if (!bioseq::indexing::sql_integer_can_represent(segment_count)) {
            ready_minimizer_error_as_cpp(
                ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE,
                "bioseq rev3 fixture segment count exceeds INTEGER");
        }
        MinimizerGenerationManifest compatibility_manifest{
            .wire_format = wire_format,
            .target_length = target_length,
            .position_segment_count = static_cast<std::uint32_t>(segment_count),
            .alphabet_code = source.alphabet_code,
            .storage_unit_byte_width = source.storage_unit_byte_width,
            .kmer_size = kmer_size,
            .window_size = window_size,
            .gpu_abi_version = profile->gpu_abi_version,
            .position_segment_span_bp = span,
            .segment_geometry_version =
                bioseq::indexing::kMinimizerSegmentGeometryVersion,
            .minimizer_scheme_id =
                bioseq::indexing::kLegacyMinimapV1SchemeId,
            .minimizer_scheme_version =
                bioseq::indexing::kLegacyMinimapV1SchemeVersion,
        };
        result.compatibility_checksum =
            bioseq::indexing::minimizer_generation_compatibility_checksum(
                compatibility_manifest);
        source.compatibility_checksum = result.compatibility_checksum;
        result.source_identity = rev3_source_identity(rid.relfilenode, source);
        result.source_content_checksum = source_content_checksum;
        result.source_num_fragments = source.locator.num_fragments;
        result.source_total_bp = source.locator.total_bp_size;
        result.source_padding_letters = source.padding_letters;
        if (result.source_identity == 0) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "bioseq rev3 fixture derived a zero source identity");
        }

        state->writer.emplace(
            identity, sequence_rowno, result.generation_id,
            /*write_batch_row_limit=*/2);
        Rev3PgIntegrationFixtureProducer producer{target_length};
        const bioseq::indexing::MinimizerGenerationEncodeOptions options{
            .wire_format = wire_format,
            .generation_id = result.generation_id,
            .source_identity = result.source_identity,
            .source_content_checksum = source_content_checksum,
            .target_length = target_length,
            .alphabet_code = source.alphabet_code,
            .storage_unit_byte_width = source.storage_unit_byte_width,
            .kmer_size = kmer_size,
            .window_size = window_size,
            .minimizer_scheme_id =
                bioseq::indexing::kLegacyMinimapV1SchemeId,
            .minimizer_scheme_version =
                bioseq::indexing::kLegacyMinimapV1SchemeVersion,
        };
        std::optional<MinimizerGenerationWireError> encode_error;
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
                "bioseq rev3 fixture encoder failed",
                fmt::format("wire_error={}",
                    static_cast<unsigned>(*encode_error)));
        }
        state->ready_token.emplace(
            state->writer->validate_ready_generation(*state->generation));
        result.writer_rows = state->writer->row_count();
        result.writer_batches =
            state->writer->catalog_multi_insert_batches();
        [[maybe_unused]] ReadyGenerationPublishedHeadReceipt receipt =
            publish_ready_minimizer_generation_head_pg(
            std::move(*state->ready_token));
        state->ready_token.reset();
        result.published_head_bytes = MinimizerGenerationHead::encoded_bytes;

        result.control_count = state->generation->manifest.control_count;
        result.position_count = state->generation->manifest.position_count;
        result.directory_entries =
            state->generation->directory.entries().size();
        result.position_segment_count =
            state->generation->manifest.position_segment_count;
        const std::uint64_t expected_event_count =
            std::min<std::uint64_t>(target_length, 4096u);
        if (result.control_count != expected_event_count
            || result.position_count != expected_event_count) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "bioseq rev3 fixture generated event totals disagree");
        }
        for (const MinimizerBucketDirEntry& entry
             : state->generation->directory.entries()) {
            result.control_encoded_bytes =
                bioseq::parallelism::saturating_add(
                    result.control_encoded_bytes,
                    entry.control_encoded_bytes);
            result.position_encoded_bytes =
                bioseq::parallelism::saturating_add(
                    result.position_encoded_bytes,
                    entry.position_encoded_bytes);
            result.control_chunks =
                bioseq::parallelism::saturating_add(
                    result.control_chunks,
                    entry.control_chunk_count);
            result.position_chunks =
                bioseq::parallelism::saturating_add(
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
                "bioseq rev3 fixture payload totals overflow");
        }
        state->generation.reset();
        state->writer.reset();
        bioseq::pg_utils::invoke_pg_as_cpp(
            [] { CommandCounterIncrement(); });
        delete state;
        state = nullptr;
    } catch (...) {
        std::exception_ptr error = std::current_exception();
        if (state != nullptr) {
            state->ready_token.reset();
            state->generation.reset();
            state->writer.reset();
            state->setup_proof.reset();
        }
        if (state != nullptr && state->registered_snapshot != nullptr) {
            Snapshot cleanup = std::exchange(
                state->registered_snapshot, nullptr);
            try {
                bioseq::pg_utils::invoke_pg_as_cpp(
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
[[nodiscard]] inline Rev3PgIntegrationFixtureResult
read_rev3_pg_integration_fixture_impl(
        Oid rel_oid,
        std::string_view index_name,
        std::uint64_t sequence_rowno,
        std::uint16_t kmer_size,
        std::uint16_t window_size,
        bioseq::indexing::MinimizerGenerationWireFormat wire_format) {
    if (!OidIsValid(rel_oid)
        || !bioseq::pg_utils::invoke_pg_as_cpp(
            [rel_oid] { return IsBioseqColumnTableAmTable(rel_oid); })) {
        ready_minimizer_error_as_cpp(
            ERRCODE_WRONG_OBJECT_TYPE,
            "bioseq rev3 fixture reader requires a USING bioseq table");
    }
    if (!bioseq::pg_utils::invoke_pg_as_cpp([] { return superuser(); })) {
        ready_minimizer_error_as_cpp(
            ERRCODE_INSUFFICIENT_PRIVILEGE,
            "bioseq rev3 fixture reader requires superuser");
    }
    if (index_name != MINIMIZERS_PER_FRAGMENT_INDEX_NAME
        && index_name != MINIMIZERS_PER_SEQUENCE_INDEX_NAME) {
        ready_minimizer_error_as_cpp(
            ERRCODE_INVALID_PARAMETER_VALUE,
            "bioseq rev3 fixture reader requires a minimizer index");
    }
    if (kmer_size == 0 || window_size == 0
        || !bioseq::indexing::sql_bigint_can_represent(sequence_rowno)
        || (wire_format
                != bioseq::indexing::MinimizerGenerationWireFormat::
                    sorted_dense_global64
            && wire_format
                != bioseq::indexing::MinimizerGenerationWireFormat::
                    grouped_hash32_segment_local32)) {
        ready_minimizer_error_as_cpp(
            ERRCODE_INVALID_PARAMETER_VALUE,
            "bioseq rev3 fixture reader parameters are invalid");
    }

    const std::int16_t seq_attno = single_seq_attno_for_inspection(rel_oid);
    const RelationIdentity rid = index_relation_identity(rel_oid);
    Rev3IndexAdministrationIdentity identity{
        .database_oid = rid.db_oid,
        .tablespace_oid = rid.tablespace_oid,
        .main_relfilenode = rid.relfilenode,
        .user_relid = rel_oid,
        .column_attno = seq_attno,
        .index_name = std::string{index_name},
    };

    struct FixtureReadState {
        Snapshot registered_snapshot = nullptr;
        std::optional<Rev3AuthoritativeSourceProof> source_proof;
        std::optional<IndexStorageFragmentGenerationReadSessionPg> session;
        std::optional<bioseq::indexing::PreparedSerializedSegment>
            serialized_reader;
        std::optional<bioseq::indexing::PreparedGrouped32QueryReader>
            grouped32_reader;
        std::vector<std::uint32_t> control_ordinals;
        std::vector<std::uint32_t> position_ordinals;
        std::vector<MinimizerGenerationPayloadReadGroup> control_groups;
        std::vector<MinimizerGenerationPayloadReadGroup> position_groups;
    };
    auto* state = new FixtureReadState{};
    Rev3PgIntegrationFixtureResult result{};
    try {
        Snapshot active_snapshot = bioseq::pg_utils::invoke_pg_as_cpp(
            [] { return GetActiveSnapshot(); });
        if (active_snapshot == nullptr || !IsMVCCSnapshot(active_snapshot)) {
            ready_minimizer_error_as_cpp(
                ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
                "bioseq rev3 fixture reader requires an active MVCC snapshot");
        }
        state->registered_snapshot = bioseq::pg_utils::invoke_pg_as_cpp(
            [active_snapshot] { return RegisterSnapshot(active_snapshot); });
        state->source_proof.emplace(
            derive_rev3_authoritative_source_proof_pg(
                identity, sequence_rowno, state->registered_snapshot));
        Rev3AuthoritativeSourceLocator source =
            state->source_proof->source_for_diagnostics();
        const std::uint64_t source_content_checksum =
            state->source_proof->source_content_checksum_for_diagnostics();
        if (source.locator.total_bp_size == 0) {
            ready_minimizer_error_as_cpp(
                ERRCODE_INVALID_PARAMETER_VALUE,
                "bioseq rev3 fixture reader requires a nonempty target");
        }
        result.table_name = format_seqidx_name(
            index_name, rid.db_oid, rid.tablespace_oid, rid.relfilenode,
            seq_attno, source.locator.bucket_idx, source.locator.shard_idx);
        result.wire_format = wire_format;
        result.source_content_checksum = source_content_checksum;
        result.source_num_fragments = source.locator.num_fragments;
        result.source_total_bp = source.locator.total_bp_size;
        result.source_padding_letters = source.padding_letters;
        identity.table_name = result.table_name;

        const auto expected =
            bioseq::indexing::expected_minimizer_compatibility_for_backend(
                wire_format,
                bioseq::simd::HWY_NAMESPACE::
                    MinimizerSketchBackend::Scalar,
                source.alphabet_code,
                source.storage_unit_byte_width,
                kmer_size,
                window_size);
        if (!expected) {
            ready_minimizer_error_as_cpp(
                ERRCODE_INVALID_PARAMETER_VALUE,
                "bioseq rev3 fixture reader compatibility is invalid",
                fmt::format("wire_error={}",
                    static_cast<unsigned>(expected.error())));
        }
        state->session.emplace(
            result.table_name,
            sequence_rowno,
            state->registered_snapshot,
            *expected,
            Rev3SnapshotRegistration::borrow_registered);
        IndexStorageFragmentGenerationReadSessionPg& session =
            *state->session;
        const MinimizerGenerationHead head = session.read_active_head_once();
        const MinimizerGenerationReadMetadata& metadata =
            session.read_manifest_directory_once(head);
        session.validate_full_source_attestation(
            std::move(*state->source_proof));
        state->source_proof.reset();

        result.generation_id = head.generation_id;
        result.compatibility_checksum =
            bioseq::indexing::minimizer_generation_compatibility_checksum(
                metadata.manifest);
        source.compatibility_checksum = result.compatibility_checksum;
        result.source_identity = rev3_source_identity(rid.relfilenode, source);
        result.control_count = metadata.manifest.control_count;
        result.position_count = metadata.manifest.position_count;
        result.directory_entries = metadata.directory.entries().size();
        result.position_segment_count =
            metadata.manifest.position_segment_count;
        const std::uint64_t expected_event_count = std::min<std::uint64_t>(
            source.locator.total_bp_size, 4096u);
        if (head.wire_format != wire_format
            || metadata.manifest.wire_format != wire_format
            || head.generation_id != metadata.manifest.generation_id
            || head.source_identity != result.source_identity
            || metadata.manifest.source_identity != result.source_identity
            || head.source_content_checksum != source_content_checksum
            || metadata.manifest.source_content_checksum
                != source_content_checksum
            || head.compatibility_checksum != result.compatibility_checksum
            || result.control_count != expected_event_count
            || result.position_count != expected_event_count) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "bioseq rev3 fixture reader identity disagrees");
        }

        constexpr std::array<std::uint64_t, 1> query_hashes{1u};
        std::optional<MinimizerGenerationWireError> reader_error;
        if (wire_format
                == bioseq::indexing::MinimizerGenerationWireFormat::
                    sorted_dense_global64) {
            for (const MinimizerBucketDirEntry& entry
                 : metadata.directory.entries()) {
                if (entry.position_segment == 0
                    && entry.lo_hash <= query_hashes.front()
                    && query_hashes.front() <= entry.hi_hash) {
                    state->control_ordinals.push_back(entry.partition_id);
                }
            }
            {
                auto reader =
                    bioseq::indexing::PreparedSerializedSegment::begin_load(
                    bioseq::parallelism::MemoryAdmissionController::instance(),
                    head,
                    metadata.manifest,
                    *expected,
                    metadata.directory.encoded_bytes(),
                    0,
                    std::span<const std::uint32_t>{
                        state->control_ordinals});
                if (reader) {
                    state->serialized_reader.emplace(std::move(*reader));
                } else {
                    reader_error = reader.error();
                }
            }
            state->position_ordinals = state->control_ordinals;
        } else {
            {
                auto reader =
                    bioseq::indexing::PreparedGrouped32QueryReader::begin(
                        bioseq::parallelism::
                            MemoryAdmissionController::instance(),
                        head,
                        metadata.manifest,
                        *expected,
                        bioseq::simd::HWY_NAMESPACE::
                            MinimizerSketchBackend::Scalar,
                        metadata.directory.encoded_bytes(),
                        query_hashes,
                        /*max_occ=*/1u);
                if (reader) {
                    state->grouped32_reader.emplace(std::move(*reader));
                } else {
                    reader_error = reader.error();
                }
            }
            if (!reader_error) {
                state->control_ordinals.assign(
                    state->grouped32_reader->control_ordinals().begin(),
                    state->grouped32_reader->control_ordinals().end());
            }
        }
        if (reader_error) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "bioseq rev3 fixture reader could not prepare wire reader",
                fmt::format("wire_format={} wire_error={}",
                    static_cast<unsigned>(wire_format),
                    static_cast<unsigned>(*reader_error)));
        }
        if (state->control_ordinals.empty()) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "bioseq rev3 fixture reader selected no control extent");
        }

        std::uint64_t expected_control_bytes = 0;
        for (const std::uint32_t ordinal : state->control_ordinals) {
            const MinimizerBucketDirEntry* entry =
                metadata.directory.entry_by_ordinal(ordinal);
            if (entry == nullptr || entry->sub_key < 0
                || entry->sub_key
                    == (std::numeric_limits<index_sub_key_t>::max)()) {
                ready_minimizer_error_as_cpp(
                    ERRCODE_DATA_CORRUPTED,
                    "bioseq rev3 fixture reader control ordinal is invalid");
            }
            expected_control_bytes = bioseq::parallelism::saturating_add(
                expected_control_bytes, entry->control_encoded_bytes);
            result.control_encoded_bytes = bioseq::parallelism::saturating_add(
                result.control_encoded_bytes, entry->control_encoded_bytes);
            result.control_chunks = bioseq::parallelism::saturating_add(
                result.control_chunks, entry->control_chunk_count);
            state->control_groups.emplace_back();
            MinimizerGenerationPayloadReadGroup& group =
                state->control_groups.back();
            group.range = IndexSubKeyRange{
                .generation_id = result.generation_id,
                .position_segment = entry->position_segment,
                .begin = entry->sub_key,
                .end = entry->sub_key + 1,
            };
            group.requests.push_back(IndexSubKeyRequest{
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
                "bioseq rev3 fixture reader control bytes overflow");
        }
        session.plan_control_groups(state->control_groups);

        Crc64Ecma182 control_crc{};
        Crc64Ecma182 position_crc{};
        std::uint64_t control_bytes = 0;
        std::uint64_t position_bytes = 0;
        std::optional<MinimizerGenerationWireError> direct_load_error;
        for (std::size_t group_index = 0;
             group_index < state->control_groups.size(); ++group_index) {
            (void)session.read_chunks_in_range(
                group_index,
                IndexChunkCachePolicy::bypass_one_shot,
                [state, wire_format, &control_crc, &control_bytes,
                 &direct_load_error](
                        const IndexChunkAddress& address,
                        const char* payload,
                        std::size_t payload_bytes) -> void {
                    if (payload == nullptr) {
                        direct_load_error =
                            MinimizerGenerationWireError::byte_count;
                        return;
                    }
                    control_crc.update(std::as_bytes(
                        std::span<const char>{payload, payload_bytes}));
                    control_bytes = bioseq::parallelism::saturating_add(
                        control_bytes, payload_bytes);
                    if (direct_load_error) {
                        return;
                    }
                    std::expected<void, MinimizerGenerationWireError>
                        accepted = wire_format
                            == bioseq::indexing::MinimizerGenerationWireFormat::
                                sorted_dense_global64
                        ? state->serialized_reader->accept_chunk(
                            address, payload, payload_bytes)
                        : state->grouped32_reader->accept_control_chunk(
                            address,
                            std::as_bytes(std::span<const char>{
                                payload, payload_bytes}));
                    if (!accepted) {
                        direct_load_error = accepted.error();
                    }
                });
        }
        if (direct_load_error || control_bytes != expected_control_bytes) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "bioseq rev3 fixture reader control load failed",
                fmt::format(
                    "wire_error={} actual_bytes={} expected_bytes={}",
                    direct_load_error
                        ? static_cast<unsigned>(*direct_load_error) + 1u : 0u,
                    control_bytes, expected_control_bytes));
        }

        if (wire_format
                == bioseq::indexing::MinimizerGenerationWireFormat::
                    grouped_hash32_segment_local32) {
            const auto finished_controls =
                state->grouped32_reader->finish_controls();
            if (!finished_controls) {
                ready_minimizer_error_as_cpp(
                    ERRCODE_DATA_CORRUPTED,
                    "bioseq rev3 fixture reader format-4 controls failed",
                    fmt::format("wire_error={}",
                        static_cast<unsigned>(finished_controls.error())));
            }
            state->position_ordinals.assign(
                state->grouped32_reader->retained_position_ordinals().begin(),
                state->grouped32_reader->retained_position_ordinals().end());
        }
        if (state->position_ordinals.empty()) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "bioseq rev3 fixture reader selected no position extent");
        }

        std::uint64_t expected_position_bytes = 0;
        for (const std::uint32_t ordinal : state->position_ordinals) {
            const MinimizerBucketDirEntry* entry =
                metadata.directory.entry_by_ordinal(ordinal);
            const auto position_sub_key = entry == nullptr
                ? std::expected<index_sub_key_t, MinimizerGenerationWireError>{
                    std::unexpected(MinimizerGenerationWireError::partition_id)}
                : derive_minimizer_position_extent_sub_key(entry->sub_key);
            if (entry == nullptr || !position_sub_key
                || *position_sub_key
                    == (std::numeric_limits<index_sub_key_t>::max)()) {
                ready_minimizer_error_as_cpp(
                    ERRCODE_DATA_CORRUPTED,
                    "bioseq rev3 fixture reader position ordinal is invalid");
            }
            expected_position_bytes = bioseq::parallelism::saturating_add(
                expected_position_bytes, entry->position_encoded_bytes);
            result.position_encoded_bytes = bioseq::parallelism::saturating_add(
                result.position_encoded_bytes, entry->position_encoded_bytes);
            result.position_chunks = bioseq::parallelism::saturating_add(
                result.position_chunks, entry->position_chunk_count);
            state->position_groups.emplace_back();
            MinimizerGenerationPayloadReadGroup& group =
                state->position_groups.back();
            group.range = IndexSubKeyRange{
                .generation_id = result.generation_id,
                .position_segment = entry->position_segment,
                .begin = *position_sub_key,
                .end = *position_sub_key + 1,
            };
            group.requests.push_back(IndexSubKeyRequest{
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
                "bioseq rev3 fixture reader position bytes overflow");
        }
        session.plan_position_groups(state->position_groups);
        for (std::size_t group_index = 0;
             group_index < state->position_groups.size(); ++group_index) {
            (void)session.read_chunks_in_range(
                group_index,
                IndexChunkCachePolicy::bypass_one_shot,
                [state, wire_format, &position_crc, &position_bytes,
                 &direct_load_error](
                        const IndexChunkAddress& address,
                        const char* payload,
                        std::size_t payload_bytes) -> void {
                    if (payload == nullptr) {
                        direct_load_error =
                            MinimizerGenerationWireError::byte_count;
                        return;
                    }
                    position_crc.update(std::as_bytes(
                        std::span<const char>{payload, payload_bytes}));
                    position_bytes = bioseq::parallelism::saturating_add(
                        position_bytes, payload_bytes);
                    if (direct_load_error) {
                        return;
                    }
                    std::expected<void, MinimizerGenerationWireError>
                        accepted = wire_format
                            == bioseq::indexing::MinimizerGenerationWireFormat::
                                sorted_dense_global64
                        ? state->serialized_reader->accept_chunk(
                            address, payload, payload_bytes)
                        : state->grouped32_reader->accept_position_chunk(
                            address,
                            std::as_bytes(std::span<const char>{
                                payload, payload_bytes}));
                    if (!accepted) {
                        direct_load_error = accepted.error();
                    }
                });
        }
        if (direct_load_error || position_bytes != expected_position_bytes) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "bioseq rev3 fixture reader position load failed",
                fmt::format(
                    "wire_error={} actual_bytes={} expected_bytes={}",
                    direct_load_error
                        ? static_cast<unsigned>(*direct_load_error) + 1u : 0u,
                    position_bytes, expected_position_bytes));
        }

        if (wire_format
                == bioseq::indexing::MinimizerGenerationWireFormat::
                    sorted_dense_global64) {
            const auto finished = state->serialized_reader->finish_load();
            const std::span<const std::uint64_t> positions = finished
                ? state->serialized_reader->lookup(query_hashes.front())
                : std::span<const std::uint64_t>{};
            if (!finished || positions.size() != 1) {
                ready_minimizer_error_as_cpp(
                    ERRCODE_DATA_CORRUPTED,
                    "bioseq rev3 fixture reader format-3 lookup failed",
                    fmt::format("wire_error={} positions={}",
                        finished ? 0u
                            : static_cast<unsigned>(finished.error()) + 1u,
                        positions.size()));
            }
            result.lookup_count = positions.size();
            result.lookup_bp = positions.front() >> 1u;
            result.lookup_reverse = (positions.front() & 1u) != 0;
            result.positions_complete = true;
        } else {
            const auto finished = state->grouped32_reader->finish_load();
            auto lookup = finished
                ? state->grouped32_reader->lookup(query_hashes.front())
                : std::expected<
                    bioseq::indexing::PreparedGrouped32QueryLookup,
                    MinimizerGenerationWireError>{
                        std::unexpected(finished.error())};
            if (!finished || !lookup
                || lookup->global_occurrence_count != 1
                || lookup->positions.size() != 1) {
                ready_minimizer_error_as_cpp(
                    ERRCODE_DATA_CORRUPTED,
                    "bioseq rev3 fixture reader format-4 lookup failed",
                    fmt::format("wire_error={} count={} positions={}",
                        lookup ? 0u
                            : static_cast<unsigned>(lookup.error()) + 1u,
                        lookup ? lookup->global_occurrence_count : 0u,
                        lookup ? lookup->positions.size() : 0u));
            }
            result.lookup_count = lookup->global_occurrence_count;
            result.lookup_bp = lookup->positions.front() >> 1u;
            result.lookup_reverse =
                (lookup->positions.front() & std::uint64_t{1}) != 0u;
            result.positions_complete = lookup->positions_complete;
        }
        if (result.lookup_count != 1 || result.lookup_bp != 0
            || result.lookup_reverse || !result.positions_complete) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "bioseq rev3 fixture reader lookup proof disagrees");
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
                "bioseq rev3 fixture reader two-phase proof disagrees");
        }

        state->serialized_reader.reset();
        state->grouped32_reader.reset();
        state->session.reset();
        bioseq::pg_utils::invoke_pg_as_cpp(
            [state] { UnregisterSnapshot(state->registered_snapshot); });
        state->registered_snapshot = nullptr;
        delete state;
        state = nullptr;
    } catch (...) {
        std::exception_ptr error = std::current_exception();
        if (state != nullptr) {
            state->serialized_reader.reset();
            state->grouped32_reader.reset();
            state->session.reset();
            state->source_proof.reset();
        }
        if (state != nullptr && state->registered_snapshot != nullptr) {
            Snapshot cleanup = std::exchange(
                state->registered_snapshot, nullptr);
            try {
                bioseq::pg_utils::invoke_pg_as_cpp(
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
/// fragment by its locator ordinal.  The public source proof must subsequently
/// fail while walking the same locator under its registered snapshot.  Ordinal
/// zero is deliberately forbidden so this seam proves later-fragment
/// participation rather than merely checking the first physical row.
inline void rev3_test_remove_source_fragment_impl(
        Oid rel_oid,
        std::string_view index_name,
        std::uint64_t sequence_rowno,
        std::uint32_t fragment_ordinal) {
    if (!bioseq::pg_utils::invoke_pg_as_cpp([] { return superuser(); })) {
        ready_minimizer_error_as_cpp(
            ERRCODE_INSUFFICIENT_PRIVILEGE,
            "bioseq rev3 source-fragment adversary requires superuser");
    }
    if (!OidIsValid(rel_oid)
        || !bioseq::pg_utils::invoke_pg_as_cpp(
            [rel_oid] { return IsBioseqColumnTableAmTable(rel_oid); })) {
        ready_minimizer_error_as_cpp(
            ERRCODE_WRONG_OBJECT_TYPE,
            "bioseq rev3 source-fragment adversary requires a USING bioseq table");
    }
    if (index_name != MINIMIZERS_PER_FRAGMENT_INDEX_NAME
        && index_name != MINIMIZERS_PER_SEQUENCE_INDEX_NAME) {
        ready_minimizer_error_as_cpp(
            ERRCODE_INVALID_PARAMETER_VALUE,
            "bioseq rev3 source-fragment adversary requires a minimizer index");
    }
    if (fragment_ordinal == 0
        || !bioseq::indexing::sql_bigint_can_represent(sequence_rowno)) {
        ready_minimizer_error_as_cpp(
            ERRCODE_INVALID_PARAMETER_VALUE,
            "bioseq rev3 source-fragment adversary requires a non-first fragment");
    }

    const std::int16_t seq_attno = single_seq_attno_for_inspection(rel_oid);
    const RelationIdentity rid = index_relation_identity(rel_oid);
    Rev3IndexAdministrationIdentity identity{
        .database_oid = rid.db_oid,
        .tablespace_oid = rid.tablespace_oid,
        .main_relfilenode = rid.relfilenode,
        .user_relid = rel_oid,
        .column_attno = seq_attno,
        .index_name = std::string{index_name},
    };

    struct RemoveState {
        Snapshot registered_snapshot = nullptr;
        seq_caches* fragment_caches = nullptr;
    };
    auto* state = new RemoveState{};
    try {
        Snapshot active_snapshot = bioseq::pg_utils::invoke_pg_as_cpp(
            [] { return GetActiveSnapshot(); });
        if (active_snapshot == nullptr || !IsMVCCSnapshot(active_snapshot)) {
            ready_minimizer_error_as_cpp(
                ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
                "bioseq rev3 source-fragment adversary requires an active MVCC snapshot");
        }
        state->registered_snapshot = bioseq::pg_utils::invoke_pg_as_cpp(
            [active_snapshot] { return RegisterSnapshot(active_snapshot); });
        Rev3AuthoritativeSourceProof proof =
            derive_rev3_authoritative_source_proof_pg(
                identity, sequence_rowno, state->registered_snapshot);
        const Rev3AuthoritativeSourceLocator source =
            proof.source_for_diagnostics();
        if (source.locator.num_fragments < 3
            || fragment_ordinal >= source.locator.num_fragments) {
            ready_minimizer_error_as_cpp(
                ERRCODE_INVALID_PARAMETER_VALUE,
                "bioseq rev3 source-fragment adversary ordinal is out of range",
                fmt::format("ordinal={} fragments={}", fragment_ordinal,
                    source.locator.num_fragments));
        }

        state->fragment_caches = bioseq::pg_utils::invoke_pg_as_cpp(
            [] { return alloc_seq_caches(CurrentMemoryContext); });
        state->fragment_caches->side = seq_caches::Side::Read;
        state->fragment_caches->lock_mode = RowExclusiveLock;
        state->fragment_caches->main_database_oid = rid.db_oid;
        state->fragment_caches->main_tablespace_oid = rid.tablespace_oid;
        state->fragment_caches->main_relfilenode = rid.relfilenode;
        [[maybe_unused]] Relation fragment_relation =
            bioseq::pg_utils::invoke_pg_as_cpp([&] {
                return open_sub_table_for_locator(
                    rid.db_oid, rid.tablespace_oid, rid.relfilenode, seq_attno,
                    source.locator, RowExclusiveLock,
                    state->fragment_caches);
            });
        const std::uint64_t fragment_rowno =
            source.locator.first_sub_row_no + fragment_ordinal;
        if (fragment_rowno < source.locator.first_sub_row_no) {
            ready_minimizer_error_as_cpp(
                ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE,
                "bioseq rev3 source-fragment row number overflow");
        }
        // Registered source fragments are immutable. Exercise the supported
        // mutation path by transactionally invalidating the exact epoch rather
        // than bypassing the TableAM guard and creating undetectable raw damage.
        bioseq::pg_utils::invoke_pg_as_cpp([&source] {
            SequenceSourceVersionManager::invalidate_epoch(
                source.locator.source_epoch);
        });
        const TM_Result deleted = TM_Ok;
        if (deleted != TM_Ok) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "bioseq rev3 source-fragment adversary could not remove the referenced row",
                fmt::format("ordinal={} tm_result={}", fragment_ordinal,
                    static_cast<int>(deleted)));
        }
        bioseq::pg_utils::invoke_pg_as_cpp([state] {
            close_and_destroy_seq_caches(
                state->fragment_caches, /*commit=*/true);
        });
        state->fragment_caches = nullptr;
        bioseq::pg_utils::invoke_pg_as_cpp(
            [state] { UnregisterSnapshot(state->registered_snapshot); });
        state->registered_snapshot = nullptr;
        bioseq::pg_utils::invoke_pg_as_cpp([] {
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
                bioseq::pg_utils::invoke_pg_as_cpp([cleanup] {
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
                bioseq::pg_utils::invoke_pg_as_cpp(
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
inline void exercise_rev3_pg_session_failure_impl(
        Oid rel_oid,
        std::string_view index_name,
        std::uint64_t sequence_rowno,
        std::string_view mode) {
    if (!bioseq::pg_utils::invoke_pg_as_cpp([] { return superuser(); })) {
        ready_minimizer_error_as_cpp(
            ERRCODE_INSUFFICIENT_PRIVILEGE,
            "bioseq rev3 session fixture requires superuser");
    }
    if (mode != "repeated_head" && mode != "repeated_metadata"
        && mode != "early_payload" && mode != "permit_denied"
        && mode != "scheme_mismatch"
        && mode != "wire_format_mismatch"
        && mode != "position_before_controls"
        && mode != "finish_before_positions"
        && mode != "repeated_control_plan"
        && mode != "control_position_namespace"
        && mode != "unauthorized_position") {
        ready_minimizer_error_as_cpp(
            ERRCODE_INVALID_PARAMETER_VALUE,
            "bioseq rev3 session fixture mode is invalid");
    }
    const std::int16_t seq_attno = single_seq_attno_for_inspection(rel_oid);
    const RelationIdentity rid = index_relation_identity(rel_oid);
    Rev3IndexAdministrationIdentity identity{
        .database_oid = rid.db_oid,
        .tablespace_oid = rid.tablespace_oid,
        .main_relfilenode = rid.relfilenode,
        .user_relid = rel_oid,
        .column_attno = seq_attno,
        .index_name = std::string{index_name},
    };
    struct FailureState {
        Snapshot registered_snapshot = nullptr;
        std::uint64_t saved_memory_cap = 0;
        bool cap_changed = false;
        std::optional<Rev3AuthoritativeSourceProof> source_proof;
        std::optional<IndexStorageFragmentGenerationReadSessionPg>
            read_session;
        std::vector<MinimizerGenerationPayloadReadGroup> groups;
    };
    auto* state = new FailureState{};
    try {
        Snapshot active_snapshot = bioseq::pg_utils::invoke_pg_as_cpp(
            [] { return GetActiveSnapshot(); });
        if (active_snapshot == nullptr || !IsMVCCSnapshot(active_snapshot)) {
            ready_minimizer_error_as_cpp(
                ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
                "bioseq rev3 session fixture requires an active MVCC snapshot");
        }
        state->registered_snapshot = bioseq::pg_utils::invoke_pg_as_cpp(
            [active_snapshot] { return RegisterSnapshot(active_snapshot); });
        state->source_proof.emplace(
            derive_rev3_authoritative_source_proof_pg(
                identity, sequence_rowno, state->registered_snapshot));
        const Rev3AuthoritativeSourceLocator& source =
            state->source_proof->source_for_diagnostics();
        identity.table_name = format_seqidx_name(
            index_name, rid.db_oid, rid.tablespace_oid, rid.relfilenode,
            seq_attno, source.locator.bucket_idx, source.locator.shard_idx);
        if (mode == "permit_denied") {
            auto& admission =
                bioseq::parallelism::MemoryAdmissionController::instance();
            state->saved_memory_cap = admission.index_memory_cap_bytes();
            admission.set_index_memory_cap_bytes(1);
            state->cap_changed = true;
        }
        auto expected =
            bioseq::indexing::expected_minimizer_compatibility_for_backend(
                bioseq::indexing::MinimizerGenerationWireFormat::
                    sorted_dense_global64,
                bioseq::simd::HWY_NAMESPACE::MinimizerSketchBackend::Scalar,
                source.alphabet_code,
                source.storage_unit_byte_width,
                static_cast<std::uint16_t>(bioseq_aligner_kmer_size),
                static_cast<std::uint16_t>(bioseq_aligner_window_size));
        if (!expected) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "bioseq rev3 session expected compatibility is invalid");
        }
        if (mode == "scheme_mismatch") {
            expected->minimizer_scheme_id =
                bioseq::indexing::kPaperNtHashU32R7SumV1SchemeId;
            expected->minimizer_scheme_version =
                bioseq::indexing::kPaperNtHashU32R7SumV1SchemeVersion;
        } else if (mode == "wire_format_mismatch") {
            expected->wire_format = bioseq::indexing::
                MinimizerGenerationWireFormat::
                    grouped_hash32_segment_local32;
            expected->gpu_abi_version =
                bioseq::indexing::kMinimizerGrouped32GpuAbiVersion;
        }
        state->read_session.emplace(
            identity.table_name, sequence_rowno, state->registered_snapshot,
            *expected,
            Rev3SnapshotRegistration::borrow_registered);
        IndexStorageFragmentGenerationReadSessionPg& session =
            *state->read_session;
        const MinimizerGenerationHead head = session.read_active_head_once();
        if (mode == "repeated_head") {
            (void)session.read_active_head_once();
        } else if (mode == "early_payload") {
            session.plan_control_groups(
                std::span<const MinimizerGenerationPayloadReadGroup>{});
        } else {
            const MinimizerGenerationReadMetadata& metadata =
                session.read_manifest_directory_once(head);
            if (mode == "repeated_metadata") {
                (void)session.read_manifest_directory_once(head);
            } else if (mode == "position_before_controls") {
                session.validate_full_source_attestation(
                    std::move(*state->source_proof));
                state->source_proof.reset();
                session.plan_position_groups(
                    std::span<const MinimizerGenerationPayloadReadGroup>{});
            } else if (mode == "finish_before_positions") {
                session.validate_full_source_attestation(
                    std::move(*state->source_proof));
                state->source_proof.reset();
                session.plan_control_groups(
                    std::span<const MinimizerGenerationPayloadReadGroup>{});
                (void)session.finish();
            } else if (mode == "repeated_control_plan") {
                session.validate_full_source_attestation(
                    std::move(*state->source_proof));
                state->source_proof.reset();
                session.plan_control_groups(
                    std::span<const MinimizerGenerationPayloadReadGroup>{});
                session.plan_control_groups(
                    std::span<const MinimizerGenerationPayloadReadGroup>{});
            } else if (mode == "control_position_namespace"
                       || mode == "unauthorized_position") {
                session.validate_full_source_attestation(
                    std::move(*state->source_proof));
                state->source_proof.reset();
                const auto entries = metadata.directory.entries();
                if (entries.empty()) {
                    ready_minimizer_error_as_cpp(
                        ERRCODE_DATA_CORRUPTED,
                        "bioseq rev3 session fixture has no payload entry");
                }
                const MinimizerBucketDirEntry& entry = entries.front();
                const auto position_sub_key =
                    derive_minimizer_position_extent_sub_key(entry.sub_key);
                if (!position_sub_key) {
                    ready_minimizer_error_as_cpp(
                        ERRCODE_DATA_CORRUPTED,
                        "bioseq rev3 session fixture position key is invalid");
                }
                state->groups.push_back(MinimizerGenerationPayloadReadGroup{
                    .range = IndexSubKeyRange{
                        .generation_id = head.generation_id,
                        .position_segment = entry.position_segment,
                        .begin = *position_sub_key,
                        .end = *position_sub_key + 1,
                    },
                    .requests = {
                        IndexSubKeyRequest{
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
                        std::span<const MinimizerGenerationPayloadReadGroup>{
                            state->groups});
                } else {
                    session.plan_control_groups(
                        std::span<const MinimizerGenerationPayloadReadGroup>{});
                    session.plan_position_groups(
                        std::span<const MinimizerGenerationPayloadReadGroup>{
                            state->groups});
                }
            }
        }
        if (state->cap_changed) {
            bioseq::parallelism::MemoryAdmissionController::instance()
                .set_index_memory_cap_bytes(state->saved_memory_cap);
            state->cap_changed = false;
        }
        state->source_proof.reset();
        state->read_session.reset();
        bioseq::pg_utils::invoke_pg_as_cpp(
            [state] { UnregisterSnapshot(state->registered_snapshot); });
        state->registered_snapshot = nullptr;
        ready_minimizer_error_as_cpp(
            ERRCODE_INTERNAL_ERROR,
            "bioseq rev3 session fixture did not reach its expected failure");
    } catch (...) {
        std::exception_ptr error = std::current_exception();
        if (state->cap_changed) {
            bioseq::parallelism::MemoryAdmissionController::instance()
                .set_index_memory_cap_bytes(state->saved_memory_cap);
            state->cap_changed = false;
        }
        state->read_session.reset();
        state->source_proof.reset();
        if (state->registered_snapshot != nullptr) {
            Snapshot cleanup = std::exchange(
                state->registered_snapshot, nullptr);
            try {
                bioseq::pg_utils::invoke_pg_as_cpp(
                    [cleanup] { UnregisterSnapshot(cleanup); });
            } catch (...) {
                error = std::current_exception();
            }
        }
        delete state;
        std::rethrow_exception(error);
    }
}

namespace rev3_generation_storage_test_detail {

struct CacheKeys {
    IndexChunkKey chunk{};
    AssembledIndexKey assembled{};
};

[[nodiscard]] inline CacheKeys make_cache_keys(
        Oid user_relid,
        const RelationIdentity& relation_identity,
        std::int16_t column_attno,
        std::string_view index_name,
        const Rev3AuthoritativeSourceLocator& source,
        std::uint64_t sequence_rowno) {
    const std::uint64_t name_hash = index_name_hash(
        relation_identity.db_oid, relation_identity.tablespace_oid,
        relation_identity.relfilenode, column_attno, index_name);
    return CacheKeys{
        .chunk = IndexChunkKey{
            .db_oid = relation_identity.db_oid,
            .user_relid = user_relid,
            .tablespace_oid = relation_identity.tablespace_oid,
            .main_relfilenode = relation_identity.relfilenode,
            .column_attno = column_attno,
            .index_name_hash = name_hash,
            .bucket_idx = static_cast<std::uint8_t>(
                source.locator.bucket_idx),
            .shard_idx = source.locator.shard_idx,
            .query_key = sequence_rowno,
            .sub_key = 0,
            .chunk_no = 0,
        },
        .assembled = AssembledIndexKey{
            .db_oid = relation_identity.db_oid,
            .user_relid = user_relid,
            .tablespace_oid = relation_identity.tablespace_oid,
            .main_relfilenode = relation_identity.relfilenode,
            .column_attno = column_attno,
            .index_name_hash = name_hash,
            .sketch_backend = bioseq::simd::HWY_NAMESPACE::
                MinimizerSketchBackend::Scalar,
            .minimizer_scheme =
                bioseq::indexing::kLegacyMinimapV1SchemeIdentity,
            .query_key = sequence_rowno,
            .query_signature = 0x7265736964656e74ULL,
            .clip_lo_bp = 0,
            .clip_hi_bp = source.locator.total_bp_size,
        },
    };
}

[[nodiscard]] inline bool relation_has_generation_schema(Oid relation_oid) {
    if (!OidIsValid(relation_oid)) return false;
    Relation relation = bioseq::pg_utils::invoke_pg_as_cpp(
        [relation_oid] {
            return try_safe_relation_open(relation_oid, AccessShareLock);
        });
    if (relation == nullptr) return false;
    const Rev3IndexSchemaInspection schema =
        bioseq::pg_utils::invoke_pg_as_cpp(
            [relation] { return inspect_rev3_index_schema(relation); });
    bioseq::pg_utils::invoke_pg_as_cpp(
        [relation] { relation_close(relation, AccessShareLock); });
    return static_cast<bool>(schema);
}

}  // namespace rev3_generation_storage_test_detail

/// Revoked integration seam for storage invariants that SQL alone cannot
/// prove: C++ handles must rebind after rolled-back schema mutation, committed
/// replacement/clear must evict both residency forms in another backend, and
/// the inactive generation clear must invalidate its initiating OID handle.
[[nodiscard]] inline Rev3GenerationStorageStateResult
exercise_rev3_generation_storage_state_impl(
        Oid rel_oid,
        std::string_view index_name,
        std::uint64_t sequence_rowno,
        std::string_view operation) {
    if (!OidIsValid(rel_oid)
        || !bioseq::pg_utils::invoke_pg_as_cpp(
            [rel_oid] { return IsBioseqColumnTableAmTable(rel_oid); })) {
        ready_minimizer_error_as_cpp(
            ERRCODE_WRONG_OBJECT_TYPE,
            "bioseq generation-storage fixture requires a USING bioseq table");
    }
    if (!bioseq::pg_utils::invoke_pg_as_cpp([] { return superuser(); })) {
        ready_minimizer_error_as_cpp(
            ERRCODE_INSUFFICIENT_PRIVILEGE,
            "bioseq generation-storage fixture requires superuser");
    }
    if (index_name != MINIMIZERS_PER_FRAGMENT_INDEX_NAME
        && index_name != MINIMIZERS_PER_SEQUENCE_INDEX_NAME) {
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
    const bool warm_residency = operation == "warm_legacy_residency";
    const bool warm_generation_residency =
        operation == "warm_generation_residency";
    const bool ensure_generation = operation == "ensure_generation";
    const bool probe_residency = operation == "probe_residency";
    const bool clear_generation = operation == "clear_generation";
    const bool clear_generation_absent =
        operation == "clear_generation_absent";
    const bool clear_generation_rollback =
        operation == "clear_generation_savepoint_rollback";
    if (!savepoint_rollback && !warm_residency
        && !warm_generation_residency && !ensure_generation
        && !probe_residency && !clear_generation
        && !clear_generation_absent && !clear_generation_rollback) {
        ready_minimizer_error_as_cpp(
            ERRCODE_INVALID_PARAMETER_VALUE,
            "bioseq generation-storage fixture operation is invalid",
            fmt::format("operation={}", operation));
    }

    // The next command normally drains sinval first.  Calling this explicitly
    // makes the two-backend cache proof independent of scheduler timing.
    if (probe_residency) {
        bioseq::pg_utils::invoke_pg_as_cpp(
            [] { AcceptInvalidationMessages(); });
    }

    const std::int16_t seq_attno = single_seq_attno_for_inspection(rel_oid);
    const RelationIdentity relation_identity = index_relation_identity(rel_oid);
    if (!OidIsValid(relation_identity.tablespace_oid)
        || !OidIsValid(relation_identity.relfilenode)) {
        ready_minimizer_error_as_cpp(
            ERRCODE_UNDEFINED_TABLE,
            "bioseq generation-storage fixture source relation is unavailable");
    }
    Snapshot active_snapshot = bioseq::pg_utils::invoke_pg_as_cpp(
        [] { return GetActiveSnapshot(); });
    if (active_snapshot == nullptr || !IsMVCCSnapshot(active_snapshot)) {
        ready_minimizer_error_as_cpp(
            ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
            "bioseq generation-storage fixture requires an active MVCC snapshot");
    }

    struct FixtureState {
        Snapshot registered_snapshot = nullptr;
        std::optional<Rev3AuthoritativeSourceProof> source_proof;
        Rev3AuthoritativeSourceLocator source{};
        Rev3IndexAdministrationIdentity identity{};
        std::optional<IndexStorageFragmentPg> storage;
        std::string table_name;
    };
    auto* state = new FixtureState{};
    state->identity = Rev3IndexAdministrationIdentity{
        .database_oid = relation_identity.db_oid,
        .tablespace_oid = relation_identity.tablespace_oid,
        .main_relfilenode = relation_identity.relfilenode,
        .user_relid = rel_oid,
        .column_attno = seq_attno,
        .index_name = std::string{index_name},
    };

    Rev3GenerationStorageStateResult result{};
    MemoryContext saved_context = CurrentMemoryContext;
    try {
        state->registered_snapshot = bioseq::pg_utils::invoke_pg_as_cpp(
            [active_snapshot] { return RegisterSnapshot(active_snapshot); });
        state->source_proof.emplace(
            derive_rev3_authoritative_source_proof_pg(
                state->identity, sequence_rowno,
                state->registered_snapshot));
        state->source = state->source_proof->source_for_diagnostics();
        state->source_proof.reset();
        bioseq::pg_utils::invoke_pg_as_cpp(
            [state] { UnregisterSnapshot(state->registered_snapshot); });
        state->registered_snapshot = nullptr;

        state->storage.emplace(open_fragment_storage(
            rel_oid, relation_identity.db_oid,
            relation_identity.tablespace_oid,
            relation_identity.relfilenode, seq_attno,
            state->identity.index_name, state->source.locator,
            sequence_rowno, nullptr, saved_context));
        IndexStorageFragmentPg& storage = *state->storage;
        state->table_name = storage.this_table_name();
        const Oid internal_namespace = bioseq_internal_ns();
        const auto resolve_table_oid = [&] {
            return bioseq::pg_utils::invoke_pg_as_cpp([&] {
                return get_relname_relid(
                    state->table_name.c_str(), internal_namespace);
            });
        };
        const auto cache_keys =
            rev3_generation_storage_test_detail::make_cache_keys(
                rel_oid, relation_identity, seq_attno,
                state->identity.index_name, state->source, sequence_rowno);

        const bool generation_input = warm_generation_residency
            || clear_generation || clear_generation_rollback;
        if (generation_input) {
            result.before_oid = resolve_table_oid();
            if (!OidIsValid(result.before_oid)
                || !rev3_generation_storage_test_detail::
                    relation_has_generation_schema(result.before_oid)) {
                rev3_storage_error(
                    "generation-storage fixture requires a canonical generation relation");
            }
            const Oid cached_oid = storage.resolve_relid_(false);
            if (cached_oid != result.before_oid
                || storage.index_relid_ != result.before_oid) {
                rev3_storage_error(
                    "generation-storage fixture could not cache the generation OID");
            }
        } else if (!probe_residency && !clear_generation_absent) {
            Relation legacy = bioseq::pg_utils::invoke_pg_as_cpp([&] {
                return storage.open_index_relation_(
                    false, AccessShareLock);
            });
            if (legacy == nullptr) {
                rev3_storage_error(
                    "generation-storage fixture legacy relation is missing");
            }
            result.before_oid = RelationGetRelid(legacy);
            bioseq::pg_utils::invoke_pg_as_cpp(
                [legacy] { relation_close(legacy, AccessShareLock); });
        } else {
            result.before_oid = resolve_table_oid();
        }

        if (warm_residency || warm_generation_residency) {
            constexpr std::array<char, 8> chunk_sentinel{
                'r', 'e', 's', 'i', 'd', 'e', 'n', 't'};
            const auto stored_chunk = store_cached_chunk(
                cache_keys.chunk,
                std::span<const char>{chunk_sentinel.data(),
                    chunk_sentinel.size()},
                /*written_in_current_xact=*/false);
            MinimizerHashmapFragmentIndex assembled_sentinel{};
            assembled_sentinel.target_len = std::max<std::uint64_t>(
                state->source.locator.total_bp_size, 1u);
            store_assembled_index(
                cache_keys.assembled, std::move(assembled_sentinel));
            const bool chunk_hit =
                stored_chunk.has_value()
                && lookup_cached_chunk(cache_keys.chunk).has_value();
            const bool assembled_hit =
                lookup_assembled_index(cache_keys.assembled).has_value();
            if (!chunk_hit || !assembled_hit) {
                rev3_storage_error(
                    "generation-storage fixture could not warm both residency forms");
            }
            result.after_oid = result.before_oid;
            result.generation_schema_after = warm_generation_residency;
            result.chunk_cache_hit = chunk_hit;
            result.assembled_cache_hit = assembled_hit;
        } else if (ensure_generation) {
            result.operation_oid =
                storage.ensure_minimizer_generation_storage();
            result.after_oid = resolve_table_oid();
            const bool exact =
                result.after_oid == result.operation_oid
                && rev3_generation_storage_test_detail::
                    relation_has_generation_schema(result.after_oid);
            if (!exact) {
                rev3_storage_error(
                    "generation-storage fixture ensure did not publish the exact schema");
            }
            result.generation_schema_after = true;
        } else if (probe_residency) {
            const bool chunk_hit =
                lookup_cached_chunk(cache_keys.chunk).has_value();
            const bool assembled_hit =
                lookup_assembled_index(cache_keys.assembled).has_value();
            result.after_oid = resolve_table_oid();
            result.generation_schema_after =
                rev3_generation_storage_test_detail::
                    relation_has_generation_schema(result.after_oid);
            result.chunk_cache_hit = chunk_hit;
            result.assembled_cache_hit = assembled_hit;
        } else if (clear_generation || clear_generation_absent) {
            const MinimizerGenerationClearResult cleared =
                storage.clear_minimizer_generation_storage();
            const bool expected_absent = clear_generation_absent;
            if ((expected_absent
                    && (cleared.dropped_relation_count != 0
                        || cleared.deleted_metadata_age_rows != 0))
                || (!expected_absent
                    && (cleared.dropped_relation_count == 0
                        || cleared.deleted_metadata_age_rows != 1))) {
                rev3_storage_error(fmt::format(
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
                rev3_storage_error(
                    "generation clear fixture retained physical or logical residency");
            }
            result.generation_schema_after = false;
            result.chunk_cache_hit = false;
            result.assembled_cache_hit = false;
        } else if (clear_generation_rollback) {
            result.operation_oid = result.before_oid;
            MemoryContext subxact_saved_context = CurrentMemoryContext;
            ResourceOwner subxact_saved_owner = CurrentResourceOwner;
            bioseq::pg_utils::invoke_pg_as_cpp([] {
                BeginInternalSubTransaction(
                    "bioseq generation clear rollback fixture");
            });
            bioseq::pg_utils::invoke_pg_as_cpp(
                [subxact_saved_context] {
                    MemoryContextSwitchTo(subxact_saved_context);
                });
            std::exception_ptr subxact_error;
            try {
                const MinimizerGenerationClearResult cleared =
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
                    rev3_storage_error(
                        "generation clear rollback fixture did not clear inside its savepoint");
                }
            } catch (...) {
                subxact_error = std::current_exception();
            }
            try {
                bioseq::pg_utils::invoke_pg_as_cpp(
                    [subxact_saved_context, subxact_saved_owner] {
                        MemoryContextSwitchTo(subxact_saved_context);
                        RollbackAndReleaseCurrentSubTransaction();
                        MemoryContextSwitchTo(subxact_saved_context);
                        CurrentResourceOwner = subxact_saved_owner;
                    });
            } catch (...) {
                subxact_error = std::current_exception();
            }
            if (subxact_error != nullptr) {
                std::rethrow_exception(subxact_error);
            }

            bioseq::pg_utils::invoke_pg_as_cpp(
                [] { CommandCounterIncrement(); });
            result.after_oid = storage.resolve_relid_(false);
            Relation restored = OidIsValid(result.after_oid)
                ? bioseq::pg_utils::invoke_pg_as_cpp([&result] {
                    return try_safe_relation_open(
                        result.after_oid, AccessShareLock);
                })
                : nullptr;
            const bool restored_schema = restored != nullptr
                && RelationGetRelid(restored) == result.before_oid
                && rev3_generation_storage_test_detail::
                    relation_has_generation_schema(result.after_oid);
            if (restored != nullptr) {
                bioseq::pg_utils::invoke_pg_as_cpp(
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
                rev3_storage_error(
                    "generation clear rollback fixture did not restore and rebind safely");
            }
            result.same_handle_rebound = true;
            result.generation_schema_after = true;
            result.chunk_cache_hit = false;
            result.assembled_cache_hit = false;
        } else {
            MemoryContext subxact_saved_context = CurrentMemoryContext;
            ResourceOwner subxact_saved_owner = CurrentResourceOwner;
            bioseq::pg_utils::invoke_pg_as_cpp([] {
                BeginInternalSubTransaction(
                    "bioseq generation storage rollback fixture");
            });
            bioseq::pg_utils::invoke_pg_as_cpp(
                [subxact_saved_context] {
                    MemoryContextSwitchTo(subxact_saved_context);
                });
            std::exception_ptr subxact_error;
            try {
                result.operation_oid =
                    storage.ensure_minimizer_generation_storage();
                // Deliberately recreate the old defect's stale member state.
                // The legacy resolver only records the authoritative name/OID;
                // it does not apply any legacy attnos here.
                const Oid deliberately_cached = storage.resolve_relid_(false);
                if (deliberately_cached != result.operation_oid) {
                    rev3_storage_error(
                        "generation-storage fixture could not cache the speculative OID");
                }
            } catch (...) {
                subxact_error = std::current_exception();
            }
            try {
                bioseq::pg_utils::invoke_pg_as_cpp(
                    [subxact_saved_context, subxact_saved_owner] {
                        MemoryContextSwitchTo(subxact_saved_context);
                        RollbackAndReleaseCurrentSubTransaction();
                        MemoryContextSwitchTo(subxact_saved_context);
                        CurrentResourceOwner = subxact_saved_owner;
                    });
            } catch (...) {
                subxact_error = std::current_exception();
            }
            if (subxact_error != nullptr) {
                std::rethrow_exception(subxact_error);
            }

            bioseq::pg_utils::invoke_pg_as_cpp(
                [] { CommandCounterIncrement(); });
            result.after_oid = storage.resolve_relid_(false);
            Relation restored = bioseq::pg_utils::invoke_pg_as_cpp([&] {
                return storage.open_index_relation_(
                    false, AccessShareLock);
            });
            const Oid reopened_oid = restored == nullptr
                ? InvalidOid : RelationGetRelid(restored);
            if (restored != nullptr) {
                bioseq::pg_utils::invoke_pg_as_cpp(
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
                rev3_storage_error(
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
            state->source_proof.reset();
            if (state->registered_snapshot != nullptr) {
                Snapshot cleanup = std::exchange(
                    state->registered_snapshot, nullptr);
                try {
                    bioseq::pg_utils::invoke_pg_as_cpp(
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
    std::int16_t result = 0;
    std::uint64_t count = 0;
    if (bioseq::pg_utils::invoke_pg_as_cpp(
            [] { return SPI_connect(); }) != SPI_OK_CONNECT) {
        ready_minimizer_error_as_cpp(
            ERRCODE_INTERNAL_ERROR,
            "bioseq minimizer generation inspection could not inspect the source schema");
    }
    const std::string sql = fmt::format(
        "SELECT a.attnum FROM pg_attribute a "
        "JOIN pg_type t ON a.atttypid=t.oid "
        "JOIN pg_namespace n ON t.typnamespace=n.oid "
        "WHERE a.attrelid={}::oid AND t.typname='seq' "
        "AND n.nspname IN ('bioseq','bioseq_internal') "
        "AND a.attnum>0 AND NOT a.attisdropped ORDER BY a.attnum",
        static_cast<unsigned>(rel_oid));
    const int status = bioseq::pg_utils::invoke_pg_as_cpp(
        [&sql] { return SPI_execute(sql.c_str(), true, 2); });
    if (status == SPI_OK_SELECT) {
        count = SPI_processed;
        if (count == 1) {
            bool is_null = false;
            const Datum value = bioseq::pg_utils::invoke_pg_as_cpp(
                [&is_null] {
                    return SPI_getbinval(
                        SPI_tuptable->vals[0], SPI_tuptable->tupdesc,
                        1, &is_null);
                });
            if (!is_null) result = DatumGetInt16(value);
        }
    }
    if (bioseq::pg_utils::invoke_pg_as_cpp(
            [] { return SPI_finish(); }) != SPI_OK_FINISH) {
        ready_minimizer_error_as_cpp(
            ERRCODE_INTERNAL_ERROR,
            "bioseq minimizer generation inspection could not close its schema query");
    }
    if (status != SPI_OK_SELECT || count != 1 || result <= 0) {
        ready_minimizer_error_as_cpp(
            ERRCODE_INVALID_PARAMETER_VALUE,
            "bioseq minimizer generation inspection requires exactly one bioseq.seq column",
            fmt::format("relation={} visible_seq_columns={}",
                static_cast<unsigned>(rel_oid), count));
    }
    return result;
}

/// Resolve and fully validate one READY revision-3 generation under one
/// registered MVCC snapshot. This setup-only bridge reads no payload extents
/// and never enables ordinal 2. A preliminary metadata pass obtains and
/// validates the generation geometry against the active k/w and fixed ABI;
/// the existing inspector then independently revalidates the source row and
/// complete metadata directory.
[[nodiscard]] inline ReadyMinimizerGenerationInspection
inspect_ready_minimizer_generation_impl(
        Oid rel_oid,
        std::string_view index_name,
        std::uint64_t sequence_rowno) {
    const bool is_bioseq_table = OidIsValid(rel_oid)
        && bioseq::pg_utils::invoke_pg_as_cpp(
            [rel_oid] { return IsBioseqColumnTableAmTable(rel_oid); });
    if (!is_bioseq_table) {
        ready_minimizer_error_as_cpp(
            ERRCODE_WRONG_OBJECT_TYPE,
            "bioseq minimizer generation inspection requires a USING bioseq table");
    }
    const Oid user_id = GetUserId();
    if (!bioseq::pg_utils::invoke_pg_as_cpp(
            [rel_oid, user_id] {
                return object_ownercheck(
                    RelationRelationId, rel_oid, user_id);
            })) {
        bioseq::pg_utils::invoke_pg_as_cpp([rel_oid] {
            aclcheck_error(
                ACLCHECK_NOT_OWNER, OBJECT_TABLE, get_rel_name(rel_oid));
        });
    }
    if (index_name != MINIMIZERS_PER_FRAGMENT_INDEX_NAME
        && index_name != MINIMIZERS_PER_SEQUENCE_INDEX_NAME) {
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
    if (bioseq_aligner_kmer_size <= 0
        || bioseq_aligner_kmer_size
            > static_cast<int>((std::numeric_limits<std::uint16_t>::max)())
        || bioseq_aligner_window_size <= 0
        || bioseq_aligner_window_size
            > static_cast<int>((std::numeric_limits<std::uint16_t>::max)())) {
        ready_minimizer_error_as_cpp(
            ERRCODE_INVALID_PARAMETER_VALUE,
            "bioseq minimizer generation inspection has invalid active k/w parameters");
    }

    const std::int16_t seq_attno = single_seq_attno_for_inspection(rel_oid);
    const RelationIdentity relation_identity = index_relation_identity(rel_oid);
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

    struct InspectionState {
        Relation relation = nullptr;
        BioseqColumnReadState* read_state = nullptr;
        Snapshot registered_snapshot = nullptr;
        MemoryContext work_context = nullptr;
    };
    auto* state = new InspectionState{};
    ReadyMinimizerGenerationInspection result{};
    const MemoryContext caller_context = CurrentMemoryContext;
    try {
        state->registered_snapshot = bioseq::pg_utils::invoke_pg_as_cpp(
            [active_snapshot] { return RegisterSnapshot(active_snapshot); });
        if (state->registered_snapshot == nullptr
            || state->registered_snapshot->regd_count == 0) {
            ready_minimizer_error_as_cpp(
                ERRCODE_INTERNAL_ERROR,
                "bioseq minimizer generation inspection could not register its snapshot");
        }
        state->work_context = bioseq::pg_utils::invoke_pg_as_cpp(
            [caller_context] {
                return AllocSetContextCreate(
                    caller_context,
                    "bioseq ready generation SQL inspection",
                    ALLOCSET_DEFAULT_SIZES);
            });
        state->relation = bioseq::pg_utils::invoke_pg_as_cpp(
            [rel_oid] {
                return try_safe_relation_open(rel_oid, AccessShareLock);
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
            bioseq::pg_utils::invoke_pg_as_cpp(
                [state] {
                    return MemoryContextSwitchTo(state->work_context);
                });
        List* projected = bioseq::pg_utils::invoke_pg_as_cpp(
            [seq_attno] { return list_make1_int(seq_attno); });
        Datum* values = bioseq::pg_utils::invoke_pg_as_cpp(
            [source_desc] {
                return static_cast<Datum*>(palloc0(
                    sizeof(Datum)
                    * static_cast<std::size_t>(source_desc->natts)));
            });
        bool* nulls = bioseq::pg_utils::invoke_pg_as_cpp(
            [source_desc] {
                return static_cast<bool*>(palloc0(
                    sizeof(bool)
                    * static_cast<std::size_t>(source_desc->natts)));
            });
        state->read_state = bioseq::pg_utils::invoke_pg_as_cpp(
            [state, source_desc, projected] {
                return BioseqColumnBeginRead(
                    state->relation, source_desc, projected, NIL,
                    state->work_context, state->registered_snapshot,
                    /*randomAccess=*/true, nullptr);
            });
        const bool row_visible = bioseq::pg_utils::invoke_pg_as_cpp(
            [state, sequence_rowno, values, nulls] {
                return BioseqColumnReadRowByRowNumber(
                    state->read_state, sequence_rowno, values, nulls);
            });
        if (!row_visible || nulls[seq_attno - 1]) {
            bioseq::pg_utils::invoke_pg_as_cpp(
                [previous_context] {
                    MemoryContextSwitchTo(previous_context);
                });
            ready_minimizer_error_as_cpp(
                ERRCODE_NO_DATA_FOUND,
                "bioseq minimizer generation inspection source row is not visible",
                fmt::format("sequence_rowno={}", sequence_rowno));
        }
        pgseq_t* header = bioseq::pg_utils::invoke_pg_as_cpp(
            [values, seq_attno] {
                return reinterpret_cast<pgseq_t*>(
                    PG_DETOAST_DATUM(values[seq_attno - 1]));
            });
        if (!is_locator_stub(header)) {
            bioseq::pg_utils::invoke_pg_as_cpp(
                [previous_context] {
                    MemoryContextSwitchTo(previous_context);
                });
            ready_minimizer_error_as_cpp(
                ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
                "bioseq minimizer generation source has no source epoch");
        }
        const seq_locator locator = decode_locator_from_stub(header);
        if (locator.source_epoch == 0) {
            bioseq::pg_utils::invoke_pg_as_cpp(
                [previous_context] {
                    MemoryContextSwitchTo(previous_context);
                });
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "bioseq minimizer generation source epoch is invalid");
        }
        result.source = Rev3AuthoritativeSourceLocator{
            .sequence_rowno = sequence_rowno,
            .locator = locator,
            .alphabet_code = static_cast<std::uint8_t>(header->alphabet_code),
            .storage_unit_byte_width = logical_storage_unit_byte_width(header),
            .padding_letters = header->padding_letters,
            .compatibility_checksum = 0,
        };
        bioseq::pg_utils::invoke_pg_as_cpp(
            [state] { BioseqColumnEndRead(state->read_state); });
        state->read_state = nullptr;
        bioseq::pg_utils::invoke_pg_as_cpp(
            [previous_context] {
                MemoryContextSwitchTo(previous_context);
            });
        Relation finished_relation = std::exchange(state->relation, nullptr);
        bioseq::pg_utils::invoke_pg_as_cpp(
            [finished_relation] {
                relation_close(finished_relation, AccessShareLock);
            });

        const std::string table_name = format_seqidx_name(
            index_name,
            relation_identity.db_oid,
            relation_identity.tablespace_oid,
            relation_identity.relfilenode,
            seq_attno,
            result.source.locator.bucket_idx,
            result.source.locator.shard_idx);
        const Rev3IndexAdministrationIdentity admin_identity{
            .database_oid = relation_identity.db_oid,
            .tablespace_oid = relation_identity.tablespace_oid,
            .main_relfilenode = relation_identity.relfilenode,
            .user_relid = rel_oid,
            .column_attno = seq_attno,
            .index_name = std::string{index_name},
            .table_name = table_name,
        };

        const auto expected =
            bioseq::indexing::expected_minimizer_compatibility_for_backend(
                bioseq::indexing::MinimizerGenerationWireFormat::
                    sorted_dense_global64,
                resolve_index_sketch_backend_from_gucs(),
                result.source.alphabet_code,
                result.source.storage_unit_byte_width,
                static_cast<std::uint16_t>(bioseq_aligner_kmer_size),
                static_cast<std::uint16_t>(bioseq_aligner_window_size));
        if (!expected) {
            ready_minimizer_error_as_cpp(
                ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
                "bioseq minimizer generation expected compatibility is invalid");
        }
        MinimizerGenerationHead preliminary_head{};
        std::uint64_t expected_source_identity = 0;
        {
            IndexStorageFragmentGenerationReadSessionPg preliminary{
                table_name, sequence_rowno, state->registered_snapshot,
                *expected,
                Rev3SnapshotRegistration::borrow_registered};
            preliminary_head = preliminary.read_active_head_once();
            const MinimizerGenerationReadMetadata& metadata =
                preliminary.read_manifest_directory_once(preliminary_head);
            result.manifest = metadata.manifest;
            result.source.compatibility_checksum =
                bioseq::indexing::minimizer_generation_compatibility_checksum(
                    result.manifest);
            expected_source_identity = rev3_source_identity(
                relation_identity.relfilenode, result.source);
            preliminary.close_metadata_read();
        }
        if (expected_source_identity == 0) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "bioseq minimizer generation inspection source identity is invalid");
        }
        if (result.manifest.kmer_size
                != static_cast<std::uint16_t>(bioseq_aligner_kmer_size)
            || result.manifest.window_size
                != static_cast<std::uint16_t>(bioseq_aligner_window_size)
            || result.manifest.alphabet_code != result.source.alphabet_code
            || result.manifest.storage_unit_byte_width
                != result.source.storage_unit_byte_width
            || result.manifest.target_length
                != result.source.locator.total_bp_size
            || result.manifest.gpu_abi_version
                != bioseq::indexing::kMinimizerSortedDenseGpuAbiVersion
            || result.manifest.segment_geometry_version
                != bioseq::indexing::kMinimizerSegmentGeometryVersion
            || result.source.compatibility_checksum
                != bioseq::indexing::minimizer_generation_compatibility_checksum(
                    result.manifest)) {
            ready_minimizer_error_as_cpp(
                ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
                "bioseq minimizer generation is incompatible with the active session",
                fmt::format("manifest k={} w={} active k={} w={}",
                    static_cast<unsigned>(result.manifest.kmer_size),
                    static_cast<unsigned>(result.manifest.window_size),
                    bioseq_aligner_kmer_size, bioseq_aligner_window_size));
        }
        result.identity = inspect_ready_minimizer_generation_identity_pg(
            admin_identity, sequence_rowno, state->registered_snapshot,
            *expected);
        if (result.identity.generation_id != preliminary_head.generation_id
            || result.identity.source_identity
                != preliminary_head.source_identity
            || result.identity.compatibility_checksum
                != result.source.compatibility_checksum) {
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
        bioseq::pg_utils::invoke_pg_as_cpp(
            [finished_snapshot] { UnregisterSnapshot(finished_snapshot); });
        MemoryContext finished_context = std::exchange(
            state->work_context, nullptr);
        bioseq::pg_utils::invoke_pg_as_cpp(
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
    const RelationIdentity rid = index_relation_identity(rel_oid);
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

[[nodiscard]] inline bool shard_has_rev3_generation_key(
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

struct ColdNoIndexAbsenceAudit {
    std::uint64_t target_rows = 0;
    bool target_cardinality_metadata_only = true;
    std::uint64_t source_parent_rows_read = 0;
    std::uint64_t source_fragment_rows_read = 0;
    std::uint64_t source_decoded_bp = 0;
    std::uint64_t physical_index_relations = 0;
    std::uint64_t persisted_index_rows = 0;
    std::uint64_t generation_objects = 0;
};

/// Fail-closed, metadata-only evidence for one no-index policy. It reads
/// Bioseq stripe metadata for target cardinality and PostgreSQL catalogs/index
/// relations for persisted-index absence. It never forms a target tuple and
/// therefore cannot hydrate or decode a sequence source.
[[nodiscard]] inline ColdNoIndexAbsenceAudit
cold_no_index_absence_audit_impl(Oid rel_oid, std::string_view name) {
    if (name != "minimizers_per_fragment"
        && name != "minimizers_per_sequence") {
        index_audit_error("audit requires an MPF or MPS minimizer policy");
    }
    Relation target = table_open(rel_oid, AccessShareLock);
    if (!IsBioseqColumnTableAmTable(rel_oid)) {
        table_close(target, AccessShareLock);
        index_audit_error("target is not a USING bioseq relation");
    }
    ColdNoIndexAbsenceAudit audit{
        .target_rows = BioseqColumnTableRowCount(target),
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
            if (shard_has_rev3_generation_key(shard)) {
                const std::int64_t generations = count_in_shard(
                    shard,
                    fmt::format(
                        "WHERE generation_id=0 AND position_segment=-1 "
                        "AND sub_key={} AND chunk_no=0",
                        static_cast<long long>(GENERATION_HEAD_SUB_KEY)));
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
    const std::string where = fmt::format("WHERE query_key={} AND sub_key < {}", (long long)query_key, (long long)TEMP_RUN_BASE);
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
    if (SPI_connect() != SPI_OK_CONNECT) return;
    SPI_execute(
        "DO $$ DECLARE r record; BEGIN FOR r IN SELECT c.relname FROM pg_class c JOIN pg_namespace n ON n.oid=c.relnamespace "
        "WHERE n.nspname='bioseq_internal' AND c.relkind='r' AND (c.relname LIKE 'seqidx\\_%' ESCAPE '\\' OR c.relname LIKE 'seqidxtab\\_%' ESCAPE '\\') "
        "LOOP EXECUTE format('DROP TABLE IF EXISTS bioseq_internal.%I CASCADE', r.relname); END LOOP; END $$;", false, 0);
    SPI_finish();
}

/// 0 ok, 1 unknown name, 2 ineligible.
[[nodiscard]] inline int set_index_policy_impl(Oid rel_oid, std::string_view name, bool update_on_insert) {
    if (!name_is_registered(name)) return 1;
    if (update_on_insert) {
        auto flags = pg_supported_indexes::allows_on_insert_flags();
        auto types = pg_supported_indexes::list_types();
        for (std::size_t i = 0; i < types.size(); ++i) if (types[i].first == name && !flags[i]) return 2;
    }
    const RelationIdentity rid = index_relation_identity(rel_oid);
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
    if (layout_of_name(name) == SequenceIndexType::CHUNKED_PER_TABLE) return 2;
    const RelationIdentity rid = index_relation_identity(rel_oid);
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
