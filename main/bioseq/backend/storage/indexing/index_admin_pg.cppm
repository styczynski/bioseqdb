/**
 * @file index_admin_pg.cppm
 * @brief DOC0279 §8.7/§11/§12 — the pg-side registry instantiation, the
 *        per-sequence storage factories, the INSERT/admin build drivers, the
 *        symbol-count reader glue, and the C++ impls behind the SQL surface.
 */
module;
#include "common.h"
#include "columnar/columnar.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/heapam.h"
#include "executor/tuptable.h"
#include "utils/snapmgr.h"
#include "utils/memutils.h"
#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
export module bioseq_storage_indexing_admin;
import mlogging;
import pg_utils;
import bioseq_storage;                     // seq_caches, seq_locator, is_locator_stub, decode_locator_from_stub
import bioseq_bucket_math;
import bioseq_storage_size;
import bioseq_storage_lazy;                // make_lazy_in_memory
import bioseq_storage_scanner_pg;          // with_lazy_postgres
import bioseq_types.pg_compact_sequence;   // pgseq_t, dispatch_instance
import bioseq_sequences;                   // compact_sequence_range
import sequan_compat;                      // seqan3::char_is_valid_for
import bioseq_indexing.type;
import bioseq_indexing.storage;
import bioseq_indexing.traits;
import bioseq_indexing.registry;
import bioseq_indexing.minimizer_hashmap;
import bioseq_indexing.minimizer_sequence;
import bioseq_indexing.symbol_counts;
import bioseq_storage_indexing_pg;
static_assert(__cplusplus >= 202302L, "C++23-or-later");

extern "C++" std::uint64_t tid_to_row_number(ItemPointerData tid);
extern "C++" bool IsColumnarTableAmTable(Oid relationId);
extern "C++" std::uint64_t ColumnarTableRowCount(Relation relation);

export namespace bioseq::storage::indexing {

using bioseq::indexing::SequenceIndexType;
using bioseq::indexing::IndexUpdatePolicy;
using bioseq::indexing::index_sub_key_t;
using bioseq::indexing::DIRECTORY_SUB_KEY;

/// The single registry of every production index (§12.1).
using pg_supported_indexes = bioseq::indexing::supported_indexes<
    IndexStorageFragmentPg,
    IndexStorageSequencePg,
    IndexStorageTablePg,
    bioseq::indexing::MinimizersPerFragmentIndex,     // CHUNKED_PER_FRAGMENT, "minimizers_per_fragment"
    bioseq::indexing::MinimizerSequenceMergedIndex,   // CHUNKED_PER_FRAGMENT, "minimizers_per_sequence"
    bioseq::indexing::SymbolCountsIndex>;             // INLINE_PER_SEQUENCE,  "symbol_counts"

// g_last_index_merge_run_count (§8.8) is defined in bioseq_storage_indexing_pg
// (same namespace, imported) so merge_runs can set it; referenced unqualified below.

// ---- relation identity ----------------------------------------------------
struct RelationIdentity { Oid db_oid, tablespace_oid, relfilenode; };
[[nodiscard]] inline RelationIdentity index_relation_identity(Oid user_relid) {
    RelationIdentity id{ MyDatabaseId, InvalidOid, InvalidOid };
    Relation r = try_safe_relation_open(user_relid, AccessShareLock);
    if (r != nullptr) {
        id.tablespace_oid = OidIsValid(r->rd_rel->reltablespace) ? r->rd_rel->reltablespace : MyDatabaseTableSpace;
        id.relfilenode = (Oid)r->rd_locator.relNumber;
        if (!OidIsValid(id.relfilenode)) id.relfilenode = RelationGetRelid(r);
        relation_close(r, AccessShareLock);
    }
    return id;
}

// ---- advisory lock (§11.4) ------------------------------------------------
inline void acquire_index_advisory_xact_lock(Oid db, Oid spc, Oid relfn, std::int16_t attno, std::string_view name) {
    std::uint64_t h = 14695981039346656037ull;
    auto mix = [&](std::uint64_t v){ for (int i=0;i<8;++i){ h ^= (v & 0xff); h *= 1099511628211ull; v >>= 8; } };
    h ^= 0xB10D2790u; mix(db); mix(spc); mix(relfn); mix((std::uint64_t)(std::uint16_t)attno);
    for (char c : name) { h ^= (std::uint8_t)c; h *= 1099511628211ull; }
    DirectFunctionCall1(pg_advisory_xact_lock_int8, Int64GetDatum((std::int64_t)h));
}

// ---- storage factories (§8.7) ---------------------------------------------
[[nodiscard]] inline MemoryContext make_handle_ctx(MemoryContext parent, const char* /*tag*/) {
    return AllocSetContextCreate(parent ? parent : CurrentMemoryContext, "bioseq_index_handle", ALLOCSET_SMALL_SIZES);
}

[[nodiscard]] inline IndexStorageFragmentPg open_fragment_storage(
        Oid user_relid, Oid db, Oid spc, Oid relfn, std::int16_t attno, std::string_view index_name,
        const seq_locator& loc, std::uint64_t sequence_rowno, seq_caches* caches, MemoryContext mctx) {
    IndexStorageFragmentPg s;
    s.db_oid_=db; s.tablespace_oid_=spc; s.main_relfilenode_=relfn; s.user_relid_=user_relid;
    s.read_caches_=caches; s.column_attno_=attno; s.index_name_=index_name;
    s.bucket_idx_=(std::uint8_t)loc.bucket_idx; s.shard_idx_=loc.shard_idx; s.query_key_=sequence_rowno;
    s.own_mctx_ = make_handle_ctx(mctx, "idx_fragment_handle");
    s.run_buffer_override_bytes_ = bioseq_index_run_buffer_bytes > 0 ? (std::size_t)bioseq_index_run_buffer_bytes : 0;
    return s;
}
[[nodiscard]] inline IndexStorageSequencePg open_sequence_storage(
        Oid user_relid, Oid db, Oid spc, Oid relfn, std::int16_t attno, std::string_view index_name,
        const seq_locator& loc, std::uint64_t sequence_rowno, seq_caches* caches, MemoryContext mctx) {
    IndexStorageSequencePg s;
    s.db_oid_=db; s.tablespace_oid_=spc; s.main_relfilenode_=relfn; s.user_relid_=user_relid;
    s.read_caches_=caches; s.column_attno_=attno; s.index_name_=index_name;
    s.bucket_idx_=(std::uint8_t)loc.bucket_idx; s.shard_idx_=loc.shard_idx; s.query_key_=sequence_rowno;
    s.own_mctx_ = make_handle_ctx(mctx, "idx_sequence_handle");
    return s;
}
[[nodiscard]] inline IndexStorageTablePg open_table_storage(
        Oid user_relid, Oid db, Oid spc, Oid relfn, std::int16_t attno, std::string_view index_name,
        seq_caches* caches, MemoryContext mctx) {
    IndexStorageTablePg s;
    s.db_oid_=db; s.tablespace_oid_=spc; s.main_relfilenode_=relfn; s.user_relid_=user_relid;
    s.read_caches_=caches; s.column_attno_=attno; s.index_name_=index_name;
    s.own_mctx_ = make_handle_ctx(mctx, "idx_table_handle");
    return s;
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
        "INSERT INTO bioseq_internal.index_age_log (user_relid, main_relfilenode, column_attno, index_name, last_updated) "
        "VALUES ({}::oid, {}::oid, {}, {}, now()) "
        "ON CONFLICT (user_relid, main_relfilenode, column_attno, index_name) DO UPDATE SET last_updated = now()",
        (unsigned)user_relid, (unsigned)relfn, (int)attno, quote_literal_cstr(std::string(name).c_str()));
    SPI_execute(sql.c_str(), false, 0);
    SPI_finish();
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
    g_last_index_merge_run_count = 0;
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
        idx.build_fragmented<true>(open_frag, lazy);
        idx.build_sequence<true>(open_inl, lazy);
    });
    // age-log the default on-insert indexes
    note_successful_write(user_relid, rid.relfilenode, attno, "minimizers_per_fragment");
    note_successful_write(user_relid, rid.relfilenode, attno, "symbol_counts");
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

// ---- resolve_active_read_index (§12.3) -----------------------------------
[[nodiscard]] inline std::string resolve_active_read_index(Oid user_relid, Oid relfn, std::int16_t attno) {
    std::string out{ bioseq::indexing::DEFAULT_ACTIVE_READ_INDEX };
    if (SPI_connect() != SPI_OK_CONNECT) return out;
    std::string sql = fmt::format(
        "SELECT index_name FROM bioseq_internal.index_policy "
        "WHERE user_relid={}::oid AND main_relfilenode={}::oid AND column_attno={} AND is_active_read AND enabled LIMIT 1",
        (unsigned)user_relid, (unsigned)relfn, (int)attno);
    if (SPI_execute(sql.c_str(), true, 0) == SPI_OK_SELECT && SPI_processed > 0) {
        char* nm = SPI_getvalue(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1);
        if (nm) out = std::string(nm);
    }
    SPI_finish();
    return out;
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

inline std::int64_t update_index_impl(Oid rel_oid, std::string_view name) {
    if (!name_is_registered(name)) pg_log(ERROR, "bioseq.update_index: unknown index '%s'", std::string(name).c_str());
    if (!IsColumnarTableAmTable(rel_oid)) pg_log(ERROR, "bioseq.update_index: relation is not a USING bioseq table");
    const RelationIdentity rid = index_relation_identity(rel_oid);
    const SequenceIndexType layout = layout_of_name(name);
    g_last_index_merge_run_count = 0;

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
    if (seq_attnos.empty()) pg_log(ERROR, "bioseq.update_index: relation has no bioseq.seq column");

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

    if (layout == SequenceIndexType::CHUNKED_PER_TABLE) {
        for (std::int16_t a : seq_attnos) {
            auto tb = open_table_storage(rel_oid, rid.db_oid, rid.tablespace_oid, rid.relfilenode, a, name, nullptr, work);
            idx.rebuild_one_table(name, tb);
        }
        MemoryContextDelete(work);
        return 0;
    }

    // per-sequence walk: build the index for every SEQ column of every row.
    seq_caches* caches = bioseq::storage::alloc_seq_caches(work);
    caches->side = bioseq::storage::seq_caches::Side::Read;
    caches->lock_mode = AccessShareLock;
    caches->read_state_mctx = work;
    caches->main_database_oid = rid.db_oid;
    caches->main_tablespace_oid = rid.tablespace_oid;
    caches->main_relfilenode = rid.relfilenode;

    Relation ur = try_safe_relation_open(rel_oid, AccessShareLock);
    TupleDesc utd = RelationGetDescr(ur);
    Snapshot snap = GetActiveSnapshot();
    TableScanDesc scan = table_beginscan_strat(ur, snap, 0, nullptr, true, true);
    TupleTableSlot* slot = MakeSingleTupleTableSlot(utd, table_slot_callbacks(ur));
    std::int64_t written = 0;
    while (table_scan_getnextslot(scan, ForwardScanDirection, slot)) {
        slot_getallattrs(slot);
        const std::uint64_t rowno = tid_to_row_number(slot->tts_tid);
        bool any_col = false;
        for (std::int16_t seq_attno : seq_attnos) {
            bool isnull=false; Datum dat = slot_getattr(slot, seq_attno, &isnull);
            if (isnull) continue;
            MemoryContext old = MemoryContextSwitchTo(work);
            pgseq_t* hdr = reinterpret_cast<pgseq_t*>(PG_DETOAST_DATUM(dat));
            MemoryContextSwitchTo(old);
            const std::uint8_t alpha = hdr->alphabet_code;
            const std::uint8_t bw = logical_storage_unit_byte_width(hdr);
            if (is_locator_stub(hdr)) {
                const seq_locator loc = decode_locator_from_stub(hdr);
                pgseq_t::dispatch_instance(alpha, bw, [&]<class S>(){
                    constexpr std::size_t lpu = compact_sequence_range<S>::traits::letters_per_unit;
                    const std::size_t pad = (loc.total_bp_size % lpu) == 0 ? 0 : (std::size_t)(lpu - loc.total_bp_size % lpu);
                    bioseq::storage::with_lazy_postgres<S>(rid.db_oid, rid.tablespace_oid, rid.relfilenode, seq_attno, loc, pad, caches, work,
                        [&](auto& lazy){
                            if (layout == SequenceIndexType::INLINE_PER_SEQUENCE) {
                                auto store = open_sequence_storage(rel_oid, rid.db_oid, rid.tablespace_oid, rid.relfilenode, seq_attno, name, loc, rowno, caches, work);
                                idx.rebuild_one_inline(name, store, lazy);
                            } else {
                                auto store = open_fragment_storage(rel_oid, rid.db_oid, rid.tablespace_oid, rid.relfilenode, seq_attno, name, loc, rowno, caches, work);
                                idx.rebuild_one_fragmented(name, store, lazy);
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
                        idx.rebuild_one_inline(name, store, lazy);
                    } else {
                        auto store = open_fragment_storage(rel_oid, rid.db_oid, rid.tablespace_oid, rid.relfilenode, seq_attno, name, loc, rowno, caches, work);
                        idx.rebuild_one_fragmented(name, store, lazy);
                    }
                });
            }
            if (reinterpret_cast<void*>(hdr) != reinterpret_cast<void*>(DatumGetPointer(dat))) pfree(hdr);
            any_col = true;
        }
        if (any_col) ++written;
    }
    ExecDropSingleTupleTableSlot(slot);
    table_endscan(scan);
    relation_close(ur, AccessShareLock);
    for (std::int16_t a : seq_attnos) note_successful_write(rel_oid, rid.relfilenode, a, name);
    bioseq::storage::destroy_seq_caches(caches);
    MemoryContextDelete(work);
    return written;
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
