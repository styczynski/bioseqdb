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
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
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

// Select the target index sketch backend from the complete algorithm tuple.
// ExactSimd is internal: the existing all-SIMD tuple selects it, and any
// unsupported alphabet/storage/vector shape fails closed in the checked driver.
// Defined before its first caller (index_build_on_insert); the spec placed it
// near layout_of_name, which is below index_build_on_insert.
[[nodiscard]] inline bioseq::simd::HWY_NAMESPACE::MinimizerSketchBackend
resolve_index_sketch_backend_from_gucs() noexcept
{
    using bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType;
    using bioseq::simd::HWY_NAMESPACE::MinimizerSketchBackend;
    using bioseq::simd::HWY_NAMESPACE::ChainingAlgorithmType;
    using bioseq::simd::HWY_NAMESPACE::BaseAlignmentAlgorithmType;
    if (bioseq_aligner_vulkan_enabled
        && bioseq_aligner_vulkan_gpu_index_build_enabled
        && bioseq_aligner_minimizer_algorithm
            == static_cast<int>(MinimizerAlgorithmType::VULKAN_MINIMAP_INT)) {
        return MinimizerSketchBackend::VulkanSegment;
    }
    if (bioseq_aligner_minimizer_algorithm
            == static_cast<int>(MinimizerAlgorithmType::TWO_STACK_NTHASH)
        && bioseq_aligner_chaining_algorithm
            == static_cast<int>(ChainingAlgorithmType::SIMD_MM2)
        && bioseq_aligner_base_alignment_algorithm
            == static_cast<int>(BaseAlignmentAlgorithmType::BLOCK_ALIGNER)) {
        return MinimizerSketchBackend::ExactSimd;
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
        idx.build_fragmented<true>(open_frag, lazy, backend);
        idx.build_sequence<true>(open_inl, lazy);
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
    return s.number_of_chunks(DIRECTORY_SUB_KEY) >= 1;
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
    std::exception_ptr cpp_error;
    PG_TRY();
    {
        // A C++ exception must not escape a PG_TRY frame: doing so skips
        // PG_END_TRY and leaves PG_exception_stack pointing at dead stack
        // storage. Capture it, restore the PG frame normally, then rethrow.
        try {
            build_fn();
        } catch (...) {
            cpp_error = std::current_exception();
        }
    }
    PG_CATCH();
    {
        ErrorData* volatile saved_error = CopyErrorData();
        FlushErrorState();
        PG_TRY();
        {
            release_runs();
        }
        PG_CATCH();
        {
            FlushErrorState();
        }
        PG_END_TRY();
        ReThrowError(saved_error);
    }
    PG_END_TRY();
    if (cpp_error != nullptr) {
        release_runs();
        std::rethrow_exception(cpp_error);
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
        idx.clear_index(f, q, tb, name);
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
        std::string_view operation,
        std::string_view detail) {
    bioseq_report<ERR, PG>()
        .code(ERRCODE_FEATURE_NOT_SUPPORTED)
        .msg("Exact SIMD minimizer index build is unsupported for this source")
        .detail("{}: {}", operation, detail)
        .hint("Exact SIMD index builds have no scalar fallback; use DNA4 with a supported packed storage width and SIMD target, or select a different minimizer algorithm explicitly.");
    __builtin_unreachable();
}

/// SQL-facing boundary: no C++ execution error may cross the PostgreSQL C ABI.
[[nodiscard]] inline std::int64_t update_index_pg_checked(
        Oid rel_oid,
        std::string_view name) {
    std::string detail;
    try {
        return update_index_impl(rel_oid, name);
    } catch (const bioseq::indexing::ExactSimdIndexRuntimeError& err) {
        detail = err.what();
    }
    // Leave the C++ catch before PostgreSQL ERROR performs its longjmp.
    raise_exact_simd_index_pg_error("bioseq.update_index", detail);
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

/// Live PostgreSQL fixture result for the revision-3 storage contract. This is
/// used only by the integration SQL seam below; production readers never call
/// it and ordinal 2 remains disabled.
struct Rev3PgIntegrationFixtureResult {
    std::string table_name;
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
    std::uint64_t validated_control_bytes = 0;
    std::uint64_t validated_position_bytes = 0;
    std::uint64_t validated_control_checksum = 0;
    std::uint64_t validated_position_checksum = 0;
    MinimizerGenerationReadStats read_stats{};
};

/// Deterministic replayable producer used by the live PG fixture. A nonempty
/// target emits one legal canonical event, which creates payload, directory and
/// manifest rows and forces two genuine CatalogTuplesMultiInsertWithInfo calls
/// when the fixture's row limit is two.
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
        if (target_length == 0) {
            return {};
        }
        return receiver(bioseq::indexing::CanonicalMinimizerEvent{
            .hash = 0x0102030405060708ULL,
            .encoded_position = 0,
        });
    }
};

static_assert(bioseq::indexing::MinimizerGenerationEventProducer<
    Rev3PgIntegrationFixtureProducer>);

/// Build, fully reread, privately tokenize, publish and reopen one exact live
/// revision-3 generation. Every write goes through the production row sink;
/// the active head goes only through the private-token publisher.
[[nodiscard]] inline Rev3PgIntegrationFixtureResult
build_rev3_pg_integration_fixture_impl(
        Oid rel_oid,
        std::string_view index_name,
        std::uint64_t sequence_rowno,
        std::uint16_t kmer_size,
        std::uint16_t window_size) {
    if (!OidIsValid(rel_oid) || !IsBioseqColumnTableAmTable(rel_oid)) {
        ereport(ERROR,
            (errcode(ERRCODE_WRONG_OBJECT_TYPE),
             errmsg("bioseq rev3 fixture requires a USING bioseq table")));
    }
    if (!superuser()) {
        ereport(ERROR,
            (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
             errmsg("bioseq rev3 fixture requires superuser")));
    }
    if (index_name != MINIMIZERS_PER_FRAGMENT_INDEX_NAME
        && index_name != MINIMIZERS_PER_SEQUENCE_INDEX_NAME) {
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg("bioseq rev3 fixture requires a minimizer index")));
    }
    if (kmer_size == 0 || window_size == 0
        || !bioseq::indexing::sql_bigint_can_represent(sequence_rowno)) {
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg("bioseq rev3 fixture parameters are invalid")));
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
        std::optional<IndexStorageFragmentGenerationReadSessionPg>
            read_session;
        std::optional<bioseq::indexing::PreparedSerializedSegment>
            prepared_segment;
        std::vector<MinimizerGenerationPayloadReadGroup> control_groups;
        std::vector<MinimizerGenerationPayloadReadGroup> position_groups;
    };
    auto* state = new FixtureState{};
    Rev3PgIntegrationFixtureResult result{};
    MemoryContext saved_context = CurrentMemoryContext;
    PG_TRY();
    {
        Snapshot active_snapshot = GetActiveSnapshot();
        if (active_snapshot == nullptr || !IsMVCCSnapshot(active_snapshot)) {
            ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("bioseq rev3 fixture requires an active MVCC snapshot")));
        }
        state->registered_snapshot = RegisterSnapshot(active_snapshot);
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
        state->setup_proof.reset();
        UnregisterSnapshot(state->registered_snapshot);
        state->registered_snapshot = nullptr;

        run_internal_ddl(fmt::format(
            "DROP TABLE IF EXISTS bioseq_internal.{}",
            quote_identifier(result.table_name.c_str())).c_str());
        run_internal_ddl(fmt::format(
            "CREATE TABLE bioseq_internal.{} ("
            "query_key BIGINT NOT NULL, generation_id BIGINT NOT NULL, "
            "position_segment INTEGER NOT NULL, sub_key BIGINT NOT NULL, "
            "chunk_no INTEGER NOT NULL, chunk_bytes INTEGER NOT NULL, "
            "checksum BIGINT NOT NULL, blob BYTEA NOT NULL, "
            "PRIMARY KEY (query_key,generation_id,position_segment,sub_key,chunk_no)) "
            "USING heap",
            quote_identifier(result.table_name.c_str())).c_str());
        run_internal_ddl(fmt::format(
            "ALTER TABLE bioseq_internal.{} ALTER COLUMN blob SET STORAGE PLAIN",
            quote_identifier(result.table_name.c_str())).c_str());
        CommandCounterIncrement();

        result.generation_id = allocate_minimizer_generation_id_pg();
        const std::uint64_t target_length = source.locator.total_bp_size;
        const std::uint64_t span = target_length == 0
            ? 0 : bioseq::indexing::kMinimizerPreferredSegmentSpanBp;
        const std::uint64_t segment_count = target_length == 0
            ? 0 : target_length / span + (target_length % span != 0 ? 1u : 0u);
        if (!bioseq::indexing::sql_integer_can_represent(segment_count)) {
            ereport(ERROR,
                (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                 errmsg("bioseq rev3 fixture segment count exceeds INTEGER")));
        }
        MinimizerGenerationManifest compatibility_manifest{
            .target_length = target_length,
            .position_segment_count = static_cast<std::uint32_t>(segment_count),
            .alphabet_code = source.alphabet_code,
            .storage_unit_byte_width = source.storage_unit_byte_width,
            .kmer_size = kmer_size,
            .window_size = window_size,
            .gpu_abi_version =
                bioseq::indexing::kMinimizerSortedDenseGpuAbiVersion,
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
            ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq rev3 fixture derived a zero source identity")));
        }

        IndexStorageFragmentGenerationRowSinkPg writer{
            identity, sequence_rowno, result.generation_id,
            /*write_batch_row_limit=*/2};
        Rev3PgIntegrationFixtureProducer producer{target_length};
        const bioseq::indexing::MinimizerGenerationEncodeOptions options{
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
        auto generation = bioseq::indexing::encode_minimizer_generation(
            producer, writer, options);
        if (!generation) {
            ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq rev3 fixture encoder failed"),
                 errdetail("wire_error=%u",
                    static_cast<unsigned>(generation.error()))));
        }
        ReadyGenerationToken ready = writer.validate_ready_generation(*generation);
        result.writer_rows = writer.row_count();
        result.writer_batches = writer.catalog_multi_insert_batches();
        publish_ready_minimizer_generation_head_pg(std::move(ready));
        result.published_head_bytes = MinimizerGenerationHead::encoded_bytes;

        CommandCounterIncrement();
        UpdateActiveSnapshotCommandId();
        state->registered_snapshot = RegisterSnapshot(GetActiveSnapshot());
        Rev3AuthoritativeSourceProof read_proof =
            derive_rev3_authoritative_source_proof_pg(
                identity, sequence_rowno, state->registered_snapshot);
        const auto expected =
            bioseq::indexing::expected_minimizer_compatibility_for_backend(
                bioseq::simd::HWY_NAMESPACE::MinimizerSketchBackend::Scalar,
                source.alphabet_code,
                source.storage_unit_byte_width,
                kmer_size,
                window_size);
        if (!expected) {
            ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq rev3 fixture expected compatibility is invalid")));
        }
        state->read_session.emplace(
            result.table_name, sequence_rowno, state->registered_snapshot,
            *expected,
            Rev3SnapshotRegistration::borrow_registered);
        IndexStorageFragmentGenerationReadSessionPg& session =
            *state->read_session;
        const MinimizerGenerationHead head = session.read_active_head_once();
        const MinimizerGenerationReadMetadata& metadata =
            session.read_manifest_directory_once(head);
        session.validate_full_source_attestation(std::move(read_proof));

        // Consume one real directory entry through the production qualified
        // reader.  Both complete extents are derived from the authenticated
        // directory (never invented by the fixture), and the callback repeats
        // the byte/CRC aggregation independently of the session's own checks.
        const auto directory_entries = metadata.directory.entries();
        if (directory_entries.size() != 1) {
            ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq rev3 fixture expected one directory entry"),
                 errdetail("entries=%zu", directory_entries.size())));
        }
        const MinimizerBucketDirEntry entry = directory_entries.front();
        const auto position_sub_key =
            derive_minimizer_position_extent_sub_key(entry.sub_key);
        if (!position_sub_key
            || entry.control_chunk_count == 0
            || entry.position_chunk_count == 0
            || entry.control_encoded_bytes == 0
            || entry.position_encoded_bytes == 0) {
            ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq rev3 fixture directory extents are invalid")));
        }
        const std::array selected_ordinals{entry.partition_id};
        auto prepared_segment =
            bioseq::indexing::PreparedSerializedSegment::begin_load(
                bioseq::parallelism::MemoryAdmissionController::instance(),
                head,
                metadata.manifest,
                *expected,
                metadata.directory.encoded_bytes(),
                entry.position_segment,
                std::span<const std::uint32_t>{selected_ordinals});
        if (!prepared_segment) {
            ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq rev3 fixture could not prepare direct serialized lookup"),
                 errdetail("wire_error=%u",
                    static_cast<unsigned>(prepared_segment.error()))));
        }
        state->prepared_segment.emplace(std::move(*prepared_segment));
        state->control_groups.push_back(
            MinimizerGenerationPayloadReadGroup{
                .range = IndexSubKeyRange{
                    .generation_id = result.generation_id,
                    .position_segment = entry.position_segment,
                    .begin = entry.sub_key,
                    .end = entry.sub_key + 1,
                },
                .requests = {
                    IndexSubKeyRequest{
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
        state->position_groups.push_back(
            MinimizerGenerationPayloadReadGroup{
                .range = IndexSubKeyRange{
                    .generation_id = result.generation_id,
                    .position_segment = entry.position_segment,
                    .begin = *position_sub_key,
                    .end = *position_sub_key + 1,
                },
                .requests = {
                    IndexSubKeyRequest{
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
        session.plan_control_groups(
            std::span<const MinimizerGenerationPayloadReadGroup>{
                state->control_groups});

        Crc64Ecma182 control_crc{};
        Crc64Ecma182 position_crc{};
        std::uint64_t control_bytes = 0;
        std::uint64_t position_bytes = 0;
        bool callback_address_valid = true;
        std::optional<MinimizerGenerationWireError> direct_load_error;
        const IndexChunkReadStats control_stats = session.read_chunks_in_range(
            0, IndexChunkCachePolicy::bypass_one_shot,
            [state, &result, &entry, &callback_address_valid,
             &control_crc, &control_bytes, &direct_load_error](
                    const IndexChunkAddress& address, const char* payload,
                    std::size_t payload_bytes) -> void {
                if (address.generation_id != result.generation_id
                    || address.position_segment != entry.position_segment
                    || address.sub_key != entry.sub_key
                    || address.chunk_no >= entry.control_chunk_count
                    || payload == nullptr) {
                    callback_address_valid = false;
                    return;
                }
                control_crc.update(std::as_bytes(
                    std::span<const char>{payload, payload_bytes}));
                control_bytes += payload_bytes;
                if (!direct_load_error) {
                    auto accepted = state->prepared_segment->accept_chunk(
                        address, payload, payload_bytes);
                    if (!accepted) {
                        direct_load_error = accepted.error();
                    }
                }
            });
        if (direct_load_error) {
            ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq rev3 fixture direct control load failed"),
                 errdetail("wire_error=%u",
                    static_cast<unsigned>(*direct_load_error))));
        }
        session.plan_position_groups(
            std::span<const MinimizerGenerationPayloadReadGroup>{
                state->position_groups});
        const IndexChunkReadStats position_stats = session.read_chunks_in_range(
            0, IndexChunkCachePolicy::bypass_one_shot,
            [state, &result, &entry, &position_sub_key,
             &callback_address_valid, &position_crc, &position_bytes,
             &direct_load_error](
                    const IndexChunkAddress& address, const char* payload,
                    std::size_t payload_bytes) -> void {
                if (address.generation_id != result.generation_id
                    || address.position_segment != entry.position_segment
                    || address.sub_key != *position_sub_key
                    || address.chunk_no >= entry.position_chunk_count
                    || payload == nullptr) {
                    callback_address_valid = false;
                    return;
                }
                position_crc.update(std::as_bytes(
                    std::span<const char>{payload, payload_bytes}));
                position_bytes += payload_bytes;
                if (!direct_load_error) {
                    auto accepted = state->prepared_segment->accept_chunk(
                        address, payload, payload_bytes);
                    if (!accepted) {
                        direct_load_error = accepted.error();
                    }
                }
            });
        if (direct_load_error) {
            ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq rev3 fixture direct position load failed"),
                 errdetail("wire_error=%u",
                    static_cast<unsigned>(*direct_load_error))));
        }
        const auto direct_finished = state->prepared_segment->finish_load();
        const auto direct_positions = direct_finished
            ? state->prepared_segment->lookup(0x0102030405060708ULL)
            : std::span<const std::uint64_t>{};
        const unsigned direct_wire_error = direct_finished
            ? 0u : static_cast<unsigned>(direct_finished.error()) + 1u;
        if (!direct_finished || direct_positions.size() != 1
            || direct_positions.front() != 0) {
            ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq rev3 fixture direct serialized lookup proof disagrees"),
                 errdetail("wire_error=%u positions=%zu encoded_position=%llu",
                    direct_wire_error,
                    direct_positions.size(),
                    static_cast<unsigned long long>(direct_positions.empty()
                        ? (std::numeric_limits<std::uint64_t>::max)()
                        : direct_positions.front()))));
        }
        result.validated_control_bytes = control_bytes;
        result.validated_position_bytes = position_bytes;
        result.validated_control_checksum = control_crc.checksum();
        result.validated_position_checksum = position_crc.checksum();
        const std::uint64_t expected_chunks =
            static_cast<std::uint64_t>(entry.control_chunk_count)
            + entry.position_chunk_count;
        if (!callback_address_valid
            || control_bytes != entry.control_encoded_bytes
            || position_bytes != entry.position_encoded_bytes
            || control_crc.checksum() != entry.control_checksum
            || position_crc.checksum() != entry.position_checksum
            || control_stats.requested_ranges != 1
            || position_stats.requested_ranges != 1
            || control_stats.wanted_subkeys != 1
            || position_stats.wanted_subkeys != 1
            || control_stats.read_subkeys != 1
            || position_stats.read_subkeys != 1
            || control_stats.wanted_extents != 1
            || position_stats.wanted_extents != 1
            || control_stats.read_extents != 1
            || position_stats.read_extents != 1
            || control_stats.wanted_chunks + position_stats.wanted_chunks
                != expected_chunks
            || control_stats.delivered_chunks + position_stats.delivered_chunks
                != expected_chunks
            || control_stats.delivered_bytes + position_stats.delivered_bytes
                != entry.control_encoded_bytes + entry.position_encoded_bytes) {
            ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq rev3 fixture payload proof disagrees")));
        }
        if (head.generation_id != result.generation_id
            || head.source_identity != result.source_identity
            || head.source_content_checksum != source_content_checksum
            || metadata.manifest.generation_id != result.generation_id) {
            ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq rev3 fixture readback identity disagrees")));
        }
        result.read_stats = session.finish();
        if (result.read_stats.controls.plan_calls != 1
            || result.read_stats.positions.plan_calls != 1
            || result.read_stats.controls.planned_request_groups != 1
            || result.read_stats.positions.planned_request_groups != 1
            || result.read_stats.controls.executed_request_groups != 1
            || result.read_stats.positions.executed_request_groups != 1
            || result.read_stats.positions.control_authorization_checks != 1
            || result.read_stats.controls.read.wanted_extents != 1
            || result.read_stats.positions.read.wanted_extents != 1) {
            ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq rev3 fixture two-phase read proof disagrees")));
        }
        state->read_session.reset();
        state->prepared_segment.reset();
        UnregisterSnapshot(state->registered_snapshot);
        state->registered_snapshot = nullptr;
        delete state;
        state = nullptr;
    }
    PG_CATCH();
    {
        MemoryContextSwitchTo(saved_context);
        ErrorData* volatile saved_error = CopyErrorData();
        FlushErrorState();
        if (state != nullptr && state->read_session.has_value()) {
            PG_TRY();
            {
                state->read_session.reset();
            }
            PG_CATCH();
            {
                FlushErrorState();
            }
            PG_END_TRY();
        }
        if (state != nullptr && state->registered_snapshot != nullptr) {
            Snapshot cleanup = std::exchange(
                state->registered_snapshot, nullptr);
            PG_TRY(); { UnregisterSnapshot(cleanup); }
            PG_CATCH(); { FlushErrorState(); }
            PG_END_TRY();
        }
        delete state;
        ReThrowError(saved_error);
    }
    PG_END_TRY();
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
    if (!superuser()) {
        ereport(ERROR,
            (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
             errmsg("bioseq rev3 source-fragment adversary requires superuser")));
    }
    if (!OidIsValid(rel_oid) || !IsBioseqColumnTableAmTable(rel_oid)) {
        ereport(ERROR,
            (errcode(ERRCODE_WRONG_OBJECT_TYPE),
             errmsg("bioseq rev3 source-fragment adversary requires a USING bioseq table")));
    }
    if (index_name != MINIMIZERS_PER_FRAGMENT_INDEX_NAME
        && index_name != MINIMIZERS_PER_SEQUENCE_INDEX_NAME) {
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg("bioseq rev3 source-fragment adversary requires a minimizer index")));
    }
    if (fragment_ordinal == 0
        || !bioseq::indexing::sql_bigint_can_represent(sequence_rowno)) {
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg("bioseq rev3 source-fragment adversary requires a non-first fragment")));
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
    MemoryContext saved_context = CurrentMemoryContext;
    PG_TRY();
    {
        Snapshot active_snapshot = GetActiveSnapshot();
        if (active_snapshot == nullptr || !IsMVCCSnapshot(active_snapshot)) {
            ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("bioseq rev3 source-fragment adversary requires an active MVCC snapshot")));
        }
        state->registered_snapshot = RegisterSnapshot(active_snapshot);
        Rev3AuthoritativeSourceProof proof =
            derive_rev3_authoritative_source_proof_pg(
                identity, sequence_rowno, state->registered_snapshot);
        const Rev3AuthoritativeSourceLocator source =
            proof.source_for_diagnostics();
        if (source.locator.num_fragments < 3
            || fragment_ordinal >= source.locator.num_fragments) {
            ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bioseq rev3 source-fragment adversary ordinal is out of range"),
                 errdetail("ordinal=%u fragments=%u", fragment_ordinal,
                    source.locator.num_fragments)));
        }

        state->fragment_caches = alloc_seq_caches(CurrentMemoryContext);
        state->fragment_caches->side = seq_caches::Side::Read;
        state->fragment_caches->lock_mode = RowExclusiveLock;
        state->fragment_caches->main_database_oid = rid.db_oid;
        state->fragment_caches->main_tablespace_oid = rid.tablespace_oid;
        state->fragment_caches->main_relfilenode = rid.relfilenode;
        Relation fragment_relation = open_sub_table_for_locator(
            rid.db_oid, rid.tablespace_oid, rid.relfilenode, seq_attno,
            source.locator, RowExclusiveLock, state->fragment_caches);
        const std::uint64_t fragment_rowno =
            source.locator.first_sub_row_no + fragment_ordinal;
        if (fragment_rowno < source.locator.first_sub_row_no) {
            ereport(ERROR,
                (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                 errmsg("bioseq rev3 source-fragment row number overflow")));
        }
        // Registered source fragments are immutable. Exercise the supported
        // mutation path by transactionally invalidating the exact epoch rather
        // than bypassing the TableAM guard and creating undetectable raw damage.
        SequenceSourceVersionManager::invalidate_epoch(
            source.locator.source_epoch);
        const TM_Result deleted = TM_Ok;
        if (deleted != TM_Ok) {
            ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq rev3 source-fragment adversary could not remove the referenced row"),
                 errdetail("ordinal=%u tm_result=%d", fragment_ordinal,
                    static_cast<int>(deleted))));
        }
        close_and_destroy_seq_caches(state->fragment_caches, /*commit=*/true);
        state->fragment_caches = nullptr;
        UnregisterSnapshot(state->registered_snapshot);
        state->registered_snapshot = nullptr;
        CommandCounterIncrement();
        UpdateActiveSnapshotCommandId();
        delete state;
        state = nullptr;
    }
    PG_CATCH();
    {
        MemoryContextSwitchTo(saved_context);
        ErrorData* volatile saved_error = CopyErrorData();
        FlushErrorState();
        if (state != nullptr && state->fragment_caches != nullptr) {
            close_and_destroy_seq_caches(
                state->fragment_caches, /*commit=*/false);
            state->fragment_caches = nullptr;
        }
        if (state != nullptr && state->registered_snapshot != nullptr) {
            UnregisterSnapshot(state->registered_snapshot);
            state->registered_snapshot = nullptr;
        }
        delete state;
        ReThrowError(saved_error);
    }
    PG_END_TRY();
}

/// Exercise read-session failure transitions against the live fixture while
/// restoring the backend memory cap and registered snapshot on every ERROR.
inline void exercise_rev3_pg_session_failure_impl(
        Oid rel_oid,
        std::string_view index_name,
        std::uint64_t sequence_rowno,
        std::string_view mode) {
    if (!superuser()) {
        ereport(ERROR,
            (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
             errmsg("bioseq rev3 session fixture requires superuser")));
    }
    if (mode != "repeated_head" && mode != "repeated_metadata"
        && mode != "early_payload" && mode != "permit_denied"
        && mode != "scheme_mismatch"
        && mode != "position_before_controls"
        && mode != "finish_before_positions"
        && mode != "repeated_control_plan"
        && mode != "control_position_namespace"
        && mode != "unauthorized_position") {
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg("bioseq rev3 session fixture mode is invalid")));
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
    MemoryContext saved_context = CurrentMemoryContext;
    PG_TRY();
    {
        state->registered_snapshot = RegisterSnapshot(GetActiveSnapshot());
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
                bioseq::simd::HWY_NAMESPACE::MinimizerSketchBackend::Scalar,
                source.alphabet_code,
                source.storage_unit_byte_width,
                static_cast<std::uint16_t>(bioseq_aligner_kmer_size),
                static_cast<std::uint16_t>(bioseq_aligner_window_size));
        if (!expected) {
            ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq rev3 session expected compatibility is invalid")));
        }
        if (mode == "scheme_mismatch") {
            expected->minimizer_scheme_id =
                bioseq::indexing::kPaperNtHashU32R7SumV1SchemeId;
            expected->minimizer_scheme_version =
                bioseq::indexing::kPaperNtHashU32R7SumV1SchemeVersion;
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
                    ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("bioseq rev3 session fixture has no payload entry")));
                }
                const MinimizerBucketDirEntry& entry = entries.front();
                const auto position_sub_key =
                    derive_minimizer_position_extent_sub_key(entry.sub_key);
                if (!position_sub_key) {
                    ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("bioseq rev3 session fixture position key is invalid")));
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
        UnregisterSnapshot(state->registered_snapshot);
        state->registered_snapshot = nullptr;
        ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("bioseq rev3 session fixture did not reach its expected failure")));
    }
    PG_CATCH();
    {
        MemoryContextSwitchTo(saved_context);
        ErrorData* volatile saved_error = CopyErrorData();
        FlushErrorState();
        if (state->cap_changed) {
            bioseq::parallelism::MemoryAdmissionController::instance()
                .set_index_memory_cap_bytes(state->saved_memory_cap);
            state->cap_changed = false;
        }
        if (state->read_session.has_value()) {
            PG_TRY();
            {
                state->read_session.reset();
            }
            PG_CATCH();
            {
                FlushErrorState();
            }
            PG_END_TRY();
        }
        if (state->registered_snapshot != nullptr) {
            Snapshot cleanup = std::exchange(
                state->registered_snapshot, nullptr);
            PG_TRY(); { UnregisterSnapshot(cleanup); }
            PG_CATCH(); { FlushErrorState(); }
            PG_END_TRY();
        }
        delete state;
        ReThrowError(saved_error);
    }
    PG_END_TRY();
}

[[nodiscard]] inline std::int16_t single_seq_attno_for_inspection(Oid rel_oid) {
    std::int16_t result = 0;
    std::uint64_t count = 0;
    if (SPI_connect() != SPI_OK_CONNECT) {
        ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("bioseq minimizer generation inspection could not inspect the source schema")));
    }
    const std::string sql = fmt::format(
        "SELECT a.attnum FROM pg_attribute a "
        "JOIN pg_type t ON a.atttypid=t.oid "
        "JOIN pg_namespace n ON t.typnamespace=n.oid "
        "WHERE a.attrelid={}::oid AND t.typname='seq' "
        "AND n.nspname IN ('bioseq','bioseq_internal') "
        "AND a.attnum>0 AND NOT a.attisdropped ORDER BY a.attnum",
        static_cast<unsigned>(rel_oid));
    const int status = SPI_execute(sql.c_str(), true, 2);
    if (status == SPI_OK_SELECT) {
        count = SPI_processed;
        if (count == 1) {
            bool is_null = false;
            const Datum value = SPI_getbinval(
                SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &is_null);
            if (!is_null) result = DatumGetInt16(value);
        }
    }
    SPI_finish();
    if (status != SPI_OK_SELECT || count != 1 || result <= 0) {
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg("bioseq minimizer generation inspection requires exactly one bioseq.seq column"),
             errdetail("relation=%u visible_seq_columns=%llu",
                 static_cast<unsigned>(rel_oid),
                 static_cast<unsigned long long>(count))));
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
    if (!OidIsValid(rel_oid) || !IsBioseqColumnTableAmTable(rel_oid)) {
        ereport(ERROR,
            (errcode(ERRCODE_WRONG_OBJECT_TYPE),
             errmsg("bioseq minimizer generation inspection requires a USING bioseq table")));
    }
    if (!object_ownercheck(RelationRelationId, rel_oid, GetUserId())) {
        aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_TABLE, get_rel_name(rel_oid));
    }
    if (index_name != MINIMIZERS_PER_FRAGMENT_INDEX_NAME
        && index_name != MINIMIZERS_PER_SEQUENCE_INDEX_NAME) {
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg("bioseq minimizer generation inspection requires a minimizer index"),
             errdetail("index_name=%.*s",
                 static_cast<int>(index_name.size()), index_name.data())));
    }
    if (!bioseq::indexing::sql_bigint_can_represent(sequence_rowno)) {
        ereport(ERROR,
            (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
             errmsg("bioseq minimizer generation inspection query key exceeds BIGINT")));
    }
    if (bioseq_aligner_kmer_size <= 0
        || bioseq_aligner_kmer_size
            > static_cast<int>((std::numeric_limits<std::uint16_t>::max)())
        || bioseq_aligner_window_size <= 0
        || bioseq_aligner_window_size
            > static_cast<int>((std::numeric_limits<std::uint16_t>::max)())) {
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg("bioseq minimizer generation inspection has invalid active k/w parameters")));
    }

    const std::int16_t seq_attno = single_seq_attno_for_inspection(rel_oid);
    const RelationIdentity relation_identity = index_relation_identity(rel_oid);
    if (!OidIsValid(relation_identity.tablespace_oid)
        || !OidIsValid(relation_identity.relfilenode)) {
        ereport(ERROR,
            (errcode(ERRCODE_UNDEFINED_TABLE),
             errmsg("bioseq minimizer generation inspection source relation is unavailable")));
    }
    Snapshot active_snapshot = GetActiveSnapshot();
    if (active_snapshot == nullptr || !IsMVCCSnapshot(active_snapshot)) {
        ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("bioseq minimizer generation inspection requires an active MVCC snapshot")));
    }

    struct InspectionState {
        Relation relation = nullptr;
        BioseqColumnReadState* read_state = nullptr;
        Snapshot registered_snapshot = nullptr;
        MemoryContext work_context = nullptr;
    };
    auto* state = new InspectionState{};
    ReadyMinimizerGenerationInspection result{};
    MemoryContext saved_context = CurrentMemoryContext;
    PG_TRY();
    {
        state->registered_snapshot = RegisterSnapshot(active_snapshot);
        if (state->registered_snapshot == nullptr
            || state->registered_snapshot->regd_count == 0) {
            ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("bioseq minimizer generation inspection could not register its snapshot")));
        }
        state->work_context = AllocSetContextCreate(
            CurrentMemoryContext, "bioseq ready generation SQL inspection",
            ALLOCSET_DEFAULT_SIZES);
        state->relation = try_safe_relation_open(rel_oid, AccessShareLock);
        if (state->relation == nullptr
            || state->relation->rd_locator.dbOid != MyDatabaseId
            || static_cast<Oid>(state->relation->rd_locator.relNumber)
                != relation_identity.relfilenode) {
            ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_TABLE),
                 errmsg("bioseq minimizer generation inspection source relation changed")));
        }

        TupleDesc source_desc = RelationGetDescr(state->relation);
        MemoryContext previous_context = MemoryContextSwitchTo(
            state->work_context);
        List* projected = list_make1_int(seq_attno);
        Datum* values = static_cast<Datum*>(palloc0(
            sizeof(Datum) * static_cast<std::size_t>(source_desc->natts)));
        bool* nulls = static_cast<bool*>(palloc0(
            sizeof(bool) * static_cast<std::size_t>(source_desc->natts)));
        state->read_state = BioseqColumnBeginRead(
            state->relation, source_desc, projected, NIL,
            state->work_context, state->registered_snapshot,
            /*randomAccess=*/true, nullptr);
        const bool row_visible = BioseqColumnReadRowByRowNumber(
            state->read_state, sequence_rowno, values, nulls);
        if (!row_visible || nulls[seq_attno - 1]) {
            MemoryContextSwitchTo(previous_context);
            ereport(ERROR,
                (errcode(ERRCODE_NO_DATA_FOUND),
                 errmsg("bioseq minimizer generation inspection source row is not visible"),
                 errdetail("sequence_rowno=%llu",
                     static_cast<unsigned long long>(sequence_rowno))));
        }
        pgseq_t* header = reinterpret_cast<pgseq_t*>(PG_DETOAST_DATUM(
            values[seq_attno - 1]));
        if (!is_locator_stub(header)) {
            MemoryContextSwitchTo(previous_context);
            ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("bioseq minimizer generation source has no source epoch")));
        }
        const seq_locator locator = decode_locator_from_stub(header);
        if (locator.source_epoch == 0) {
            MemoryContextSwitchTo(previous_context);
            ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq minimizer generation source epoch is invalid")));
        }
        result.source = Rev3AuthoritativeSourceLocator{
            .sequence_rowno = sequence_rowno,
            .locator = locator,
            .alphabet_code = static_cast<std::uint8_t>(header->alphabet_code),
            .storage_unit_byte_width = logical_storage_unit_byte_width(header),
            .padding_letters = header->padding_letters,
            .compatibility_checksum = 0,
        };
        BioseqColumnEndRead(state->read_state);
        state->read_state = nullptr;
        MemoryContextSwitchTo(previous_context);
        Relation finished_relation = std::exchange(state->relation, nullptr);
        relation_close(finished_relation, AccessShareLock);

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
                resolve_index_sketch_backend_from_gucs(),
                result.source.alphabet_code,
                result.source.storage_unit_byte_width,
                static_cast<std::uint16_t>(bioseq_aligner_kmer_size),
                static_cast<std::uint16_t>(bioseq_aligner_window_size));
        if (!expected) {
            ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("bioseq minimizer generation expected compatibility is invalid")));
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
        }
        if (expected_source_identity == 0) {
            ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq minimizer generation inspection source identity is invalid")));
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
            ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("bioseq minimizer generation is incompatible with the active session"),
                 errdetail("manifest k=%u w=%u active k=%d w=%d",
                     static_cast<unsigned>(result.manifest.kmer_size),
                     static_cast<unsigned>(result.manifest.window_size),
                     bioseq_aligner_kmer_size, bioseq_aligner_window_size)));
        }
        result.identity = inspect_ready_minimizer_generation_identity_pg(
            admin_identity, sequence_rowno, state->registered_snapshot,
            *expected);
        if (result.identity.generation_id != preliminary_head.generation_id
            || result.identity.source_identity
                != preliminary_head.source_identity
            || result.identity.compatibility_checksum
                != result.source.compatibility_checksum) {
            ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq minimizer generation identity changed during inspection")));
        }
        result.source_content_checksum =
            result.identity.source_content_checksum;
        result.database_oid = relation_identity.db_oid;
        result.main_relfilenode = relation_identity.relfilenode;
        result.column_attno = seq_attno;

        Snapshot finished_snapshot = std::exchange(
            state->registered_snapshot, nullptr);
        UnregisterSnapshot(finished_snapshot);
        MemoryContext finished_context = std::exchange(
            state->work_context, nullptr);
        MemoryContextDelete(finished_context);
        delete state;
        state = nullptr;
    }
    PG_CATCH();
    {
        MemoryContextSwitchTo(saved_context);
        ErrorData* volatile saved_error = CopyErrorData();
        FlushErrorState();
        if (state->read_state != nullptr) {
            BioseqColumnReadState* cleanup = std::exchange(
                state->read_state, nullptr);
            PG_TRY(); { BioseqColumnEndRead(cleanup); }
            PG_CATCH(); { FlushErrorState(); }
            PG_END_TRY();
        }
        if (state->relation != nullptr) {
            Relation cleanup = std::exchange(state->relation, nullptr);
            PG_TRY(); { relation_close(cleanup, AccessShareLock); }
            PG_CATCH(); { FlushErrorState(); }
            PG_END_TRY();
        }
        if (state->registered_snapshot != nullptr) {
            Snapshot cleanup = std::exchange(
                state->registered_snapshot, nullptr);
            PG_TRY(); { UnregisterSnapshot(cleanup); }
            PG_CATCH(); { FlushErrorState(); }
            PG_END_TRY();
        }
        if (state->work_context != nullptr) {
            MemoryContext cleanup = std::exchange(
                state->work_context, nullptr);
            PG_TRY(); { MemoryContextDelete(cleanup); }
            PG_CATCH(); { FlushErrorState(); }
            PG_END_TRY();
        }
        delete state;
        ReThrowError(saved_error);
    }
    PG_END_TRY();
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
