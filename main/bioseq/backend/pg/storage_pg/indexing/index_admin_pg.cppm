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
#include "access/heapam.h"
#include "executor/tuptable.h"
#include "utils/snapmgr.h"
#include "utils/memutils.h"
#include <algorithm>
#include <array>
#include <concepts>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
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
using bioseq::indexing::TEMP_RUN_BASE;

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

// ---- advisory lock (§11.4) ------------------------------------------------
inline void acquire_index_advisory_xact_lock(Oid db, Oid spc, Oid relfn, std::int16_t attno, std::string_view name) {
    std::uint64_t h = 14695981039346656037ull;
    auto mix = [&](std::uint64_t v){ for (int i=0;i<8;++i){ h ^= (v & 0xff); h *= 1099511628211ull; v >>= 8; } };
    h ^= 0xB10D2790u; mix(db); mix(spc); mix(relfn); mix((std::uint64_t)(std::uint16_t)attno);
    for (char c : name) { h ^= (std::uint8_t)c; h *= 1099511628211ull; }
    DirectFunctionCall1(pg_advisory_xact_lock_int8, Int64GetDatum((std::int64_t)h));
}

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

// DOC0345 Patch I — select the target index sketch backend from the three
// high-level GUCs (vulkan enabled, GPU index build enabled, vulkan_minimap_int).
// Defined before its first caller (index_build_on_insert); the spec placed it
// near layout_of_name, which is below index_build_on_insert.
[[nodiscard]] inline bioseq::simd::HWY_NAMESPACE::MinimizerSketchBackend
resolve_index_sketch_backend_from_gucs() noexcept
{
    using bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType;
    using bioseq::simd::HWY_NAMESPACE::MinimizerSketchBackend;
    return bioseq_aligner_vulkan_enabled
        && bioseq_aligner_vulkan_gpu_index_build_enabled
        && bioseq_aligner_minimizer_algorithm
            == static_cast<int>(MinimizerAlgorithmType::VULKAN_MINIMAP_INT)
        ? MinimizerSketchBackend::VulkanSegment
        : MinimizerSketchBackend::Scalar;
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
    PG_TRY();
    {
        build_fn();
    }
    PG_CATCH();
    {
        ErrorData* volatile saved_error = CopyErrorData();
        FlushErrorState();
        PG_TRY();
        {
            if constexpr (requires (Storage& cleanup_storage) {
                    { cleanup_storage.release_runs_noexcept() } noexcept -> std::same_as<void>;
                }) {
                storage.release_runs_noexcept();
            } else if constexpr (requires (Storage& cleanup_storage) {
                    { cleanup_storage.release_resident_runs_noexcept() } noexcept -> std::same_as<void>;
                }) {
                storage.release_resident_runs_noexcept();
            }
        }
        PG_CATCH();
        {
            FlushErrorState();
        }
        PG_END_TRY();
        ReThrowError(saved_error);
    }
    PG_END_TRY();
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
    // single varlena". BioseqColumnReadNextRow returns the RAW stored value (the 48-byte
    // locator stub for a large sequence); we decode the locator and STREAM fragments
    // via scanner_pg (RSS bounded by one fragment) — the only path that can index a
    // >MaxAllocSize target. `&rowno` is the columnar row number == query_key (no
    // tid_to_row_number needed; that path also fixed the rowno source).
    Snapshot snap = GetTransactionSnapshot();
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
                    bioseq::storage::with_lazy_postgres<S>(rid.db_oid, rid.tablespace_oid, rid.relfilenode, seq_attno, loc, pad, caches, inner,
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
    BioseqColumnEndRead(rs);
    MemoryContextDelete(inner);
    relation_close(ur, AccessShareLock);
    for (std::int16_t a : seq_attnos) note_successful_write(rel_oid, rid.relfilenode, a, name);
    caches->close_all(/*commit=*/false);   // close cached seq_sub read-state relations (else "resource was not closed" at commit)
    bioseq::storage::destroy_seq_caches(caches);
    MemoryContextDelete(work);
    return written;
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

[[nodiscard]] inline std::vector<std::string> seqidx_prefixes_for(Oid rel_oid, std::string_view name) {
    const RelationIdentity rid = index_relation_identity(rel_oid);
    std::vector<std::string> prefixes;
    if (SPI_connect() != SPI_OK_CONNECT) return prefixes;
    std::string sql = fmt::format(
        "SELECT a.attnum FROM pg_attribute a JOIN pg_type t ON a.atttypid=t.oid JOIN pg_namespace n ON t.typnamespace=n.oid "
        "WHERE a.attrelid={}::oid AND t.typname='seq' AND n.nspname IN ('bioseq','bioseq_internal') AND a.attnum>0 AND NOT a.attisdropped",
        (unsigned)rel_oid);
    if (SPI_execute(sql.c_str(), true, 0) == SPI_OK_SELECT) {
        for (std::uint64_t r = 0; r < SPI_processed; ++r) {
            bool isn=false; std::int16_t a = (std::int16_t)DatumGetInt16(SPI_getbinval(SPI_tuptable->vals[r], SPI_tuptable->tupdesc, 1, &isn));
            prefixes.push_back(fmt::format("seqidx_{}_{:016x}_", short_index_for(name),
                identity_hash64(rid.db_oid, rid.tablespace_oid, rid.relfilenode, a, name)));
        }
    }
    SPI_finish();
    return prefixes;
}

[[nodiscard]] inline std::vector<std::string> shard_relnames_with_prefix(const std::string& pfx) {
    std::vector<std::string> out;
    if (SPI_connect() != SPI_OK_CONNECT) return out;
    std::string esc; for (char c : pfx) { if (c=='_'||c=='%'||c=='\\') esc.push_back('\\'); esc.push_back(c); }
    std::string sql = fmt::format(
        "SELECT c.relname FROM pg_class c JOIN pg_namespace n ON n.oid=c.relnamespace "
        "WHERE n.nspname='bioseq_internal' AND c.relkind='r' AND c.relname LIKE '{}%' ESCAPE '\\'", esc);
    if (SPI_execute(sql.c_str(), true, 0) == SPI_OK_SELECT)
        for (std::uint64_t r = 0; r < SPI_processed; ++r) {
            char* nm = SPI_getvalue(SPI_tuptable->vals[r], SPI_tuptable->tupdesc, 1);
            if (nm) out.emplace_back(nm);
        }
    SPI_finish();
    return out;
}

[[nodiscard]] inline std::int64_t count_in_shard(const std::string& relname, const std::string& where_clause) {
    std::int64_t v = 0;
    if (SPI_connect() != SPI_OK_CONNECT) return 0;
    std::string sql = fmt::format("SELECT count(*)::bigint FROM bioseq_internal.{} {}", quote_identifier(relname.c_str()), where_clause);
    if (SPI_execute(sql.c_str(), true, 0) == SPI_OK_SELECT && SPI_processed > 0) {
        bool isn=false; Datum d = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isn);
        if (!isn) v = DatumGetInt64(d);
    }
    SPI_finish();
    return v;
}

[[nodiscard]] inline std::int64_t index_table_row_count_impl(Oid rel_oid, std::string_view name) {
    std::int64_t total = 0;
    for (const std::string& pfx : seqidx_prefixes_for(rel_oid, name))
        for (const std::string& sh : shard_relnames_with_prefix(pfx))
            total += count_in_shard(sh, "");
    return total;
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
