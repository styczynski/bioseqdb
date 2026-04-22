/*-------------------------------------------------------------------------
 *
 * scheduler/dispatch_buffer.cppm
 *
 * TupleDispatchBuffer accumulates TupleTableSlot copies produced during
 * a custom-scan execution phase so the scan node can later replay them in
 * order. The scan runs in two phases per query:
 *
 *   Writing phase (driven by submit_scan): for every source tuple that
 *   passed the WHERE qual, the scan copies the slot into the buffer and
 *   records the row's primary key and a detoasted copy of its bioseq.seq
 *   varlena. Projection does NOT happen here; `ecxt_scantuple` still
 *   references the original scan slot which is about to be overwritten
 *   by the next call.
 *
 *   Batch phase (run_batch_and_build_cursors): after the access method
 *   signals EOF, the buffer feeds its target varlenas (plus the mmsearch
 *   query varlena captured at BeginCustomScan) into
 *   BatchProcessor::process_sequences. That function dispatches on the
 *   column's (alphabet_code, width_code) typmod to instantiate the right
 *   compact_sequence_range<Dst> specialisation, then invokes
 *   minimap_port(query, target) for every row and stores the returned
 *   MatchResults keyed by primary key. Rows whose PK is missing from the
 *   returned map are filtered; this is how WHERE mmsearch(...) evaluates
 *   to FALSE for those rows.
 *
 *   Reading phase (read_next_output): the custom scan iterates over
 *   output cursors, pointing `ecxt_scantuple` at the buffered scan slot
 *   and projecting via ExecProject. The placeholder Const* nodes
 *   injected by VarInjectorMutator are updated from the MatchResult
 *   before projection fires.
 *
 *-------------------------------------------------------------------------
 */
module;

#include <hwy/highway.h>
#include "common.h"

#include <array>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

/* Forward-declared so we can stay out of bioseq_table (the module that
 * defines tid_to_row_number). Pulling the full module in would
 * reintroduce a circular import between the scheduler and the table AM.
 * The `extern "C++"` attaches the declaration to the global module
 * fragment, matching the attachment of the definition in
 * access_method.cppm. */
extern "C++" uint64_t tid_to_row_number(ItemPointerData tid);

export module bioseq_scheduler.dispatch_buffer;

import mlogging;
import pg_utils;
import bioseq_sequences;
import bioseq_sequences.minimap_port;
import bioseq_cigar;
import bioseq_types.pg_compact_sequence;

HWY_BEFORE_NAMESPACE();

namespace bioseq::simd::HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

// Fixed upper bound on a single batch. Matches the TupleDispatchBuffer
// constructor hint; entries beyond this get truncated with a log line.
constexpr size_t kDispatchBatchCapacity = 1000;

/*
 * BatchProcessor drives minimap_port across a batch of (query, target)
 * pairs that all share the same (alphabet_code, width_code) typmod. The
 * caller hands us the already-detoasted query varlena (captured at
 * BeginCustomScan — see columnar_customscan.cppm) and an array of per-row
 * target varlenas (captured at submit_scan — see TupleDispatchBuffer).
 *
 * Dispatch: we branch on (alphabet_code, width_code) exactly once, then
 * inside the matching arm all eight compact_sequence_range<Dst>
 * specialisations compile down to their own minimap_port instantiation.
 * No std::variant, no runtime type erasure on the hot path.
 */
export class BatchProcessor {
public:
    template<typename K, size_t batch_capacity>
    [[nodiscard]] std::map<K, std::vector<MatchResult>> process_sequences(
        const size_t batch_size,
        const uint8_t alphabet_code,
        const uint8_t width_code,
        const PgCompactSequenceHdr* query_hdr,
        const std::array<const PgCompactSequenceHdr*, batch_capacity>& target_hdrs,
        const std::array<K, batch_capacity>& query_ids)
    {
        std::map<K, std::vector<MatchResult>> results;

        if (query_hdr == nullptr) {
            JustLog("[bioseq] pg: BatchProcessor::process_sequences received null query_hdr; returning empty map");
            return results;
        }
        if (query_hdr->alphabet_code != alphabet_code || query_hdr->width_code != width_code) {
            JustLog("[bioseq] pg: BatchProcessor::process_sequences query_hdr typmod ({},{}) disagrees with context ({},{}); refusing to dispatch",
                    static_cast<unsigned>(query_hdr->alphabet_code),
                    static_cast<unsigned>(query_hdr->width_code),
                    static_cast<unsigned>(alphabet_code),
                    static_cast<unsigned>(width_code));
            return results;
        }

        dispatch_pgseq(alphabet_code, width_code,
            [&]<class Dst>() {
                /*
                 * Build the per-batch query compact_sequence_range once
                 * from the stashed PG varlena. make_seq_range_from_pg
                 * validates the header and copies the payload into an
                 * owned std::vector — the alloc is amortised across
                 * every target in this batch.
                 */
                compact_sequence_range<Dst> query_seq = make_seq_range_from_pg<Dst>(query_hdr);
                JustLog("[bioseq] pg: BatchProcessor dispatching ({},{}) query_bp_size={} batch_size={}",
                        static_cast<unsigned>(alphabet_code),
                        static_cast<unsigned>(width_code),
                        query_seq.bp_size(), batch_size);

                for (size_t i = 0; i < batch_size; ++i) {
                    const PgCompactSequenceHdr* target_hdr = target_hdrs[i];
                    if (target_hdr == nullptr) {
                        continue;  // NULL seq column in this row → no match
                    }
                    if (target_hdr->alphabet_code != alphabet_code
                        || target_hdr->width_code != width_code) {
                        /*
                         * PG's typmod coercion should have rejected the
                         * INSERT if the stored typmod didn't match the
                         * column typmod; getting here means someone
                         * wrote raw bytes via pg_catalog mischief or a
                         * future cross-typmod compare path. Skip rather
                         * than crash and let the tuple's mmsearch
                         * qualify as FALSE.
                         */
                        JustLog("[bioseq] pg: target row query_id={} has typmod ({},{}) mismatching context ({},{}); treating as WHERE FALSE",
                                static_cast<int>(query_ids[i]),
                                static_cast<unsigned>(target_hdr->alphabet_code),
                                static_cast<unsigned>(target_hdr->width_code),
                                static_cast<unsigned>(alphabet_code),
                                static_cast<unsigned>(width_code));
                        continue;
                    }
                    compact_sequence_range<Dst> target_seq = make_seq_range_from_pg<Dst>(target_hdr);

                    /*
                     * Tune algorithm parameters for the query/target size
                     * regime we are likely to see. minimap2's hard-coded
                     * defaults (k=15, w=10, min_chain_score=40,
                     * min_seed_count=3) are calibrated for long-read vs
                     * genome mapping (kb-scale query against Mb-scale
                     * target). In a Postgres context the query literal is
                     * routinely short (a probe motif, a primer, an amplicon
                     * region) against per-row targets typically <1 kb, so
                     * we scale k/w down proportionally. When both operands
                     * are long we keep the original minimap2 defaults.
                     */
                    Minimap2Params params;
                    const size_t min_bp = std::min(query_seq.bp_size(),
                                                   target_seq.bp_size());
                    if (min_bp < 50) {
                        params.kmer_size       = 8;
                        params.window_size     = 3;
                        params.min_chain_score = 10;
                        params.min_seed_count  = 1;
                    } else if (min_bp < 200) {
                        params.kmer_size       = 10;
                        params.window_size     = 5;
                        params.min_chain_score = 20;
                        params.min_seed_count  = 2;
                    }
                    MatchResultCollection collection = minimap_port(query_seq, target_seq, params);
                    JustLog("[bioseq] pg: minimap_port query_id={} target_bp_size={} k={} w={} min_chain_score={} min_seed_count={} produced {} match(es)",
                            static_cast<int>(query_ids[i]),
                            target_seq.bp_size(),
                            params.kmer_size, params.window_size,
                            params.min_chain_score, params.min_seed_count,
                            collection.size());
                    if (!collection.empty()) {
                        results[query_ids[i]] = std::move(collection.results);
                    }
                }
            });

        return results;
    }
};

export class TupleDispatchBuffer {
public:
    /*
     * Context captured at BeginCustomScan from cscan->scan.plan.qual /
     * targetlist. seq_attno is the 1-based attribute number of the Var
     * passed as the first argument of mmsearch(); primary_key_attno is
     * the 1-based attribute number of the Var passed as the argument of
     * any bioseq.<field>(...) projection function. alphabet_code /
     * width_code hold the (alphabet, width) typmod shared by the column
     * and the query literal — they are validated to be equal at
     * BeginCustomScan time. query_varlena holds a full detoasted copy
     * of the mmsearch second-argument bioseq.seq Const, so submit_scan
     * never has to re-read from the plan tree.
     */
    struct MmsearchContext {
        AttrNumber seq_attno           = InvalidAttrNumber;
        AttrNumber primary_key_attno   = InvalidAttrNumber;
        std::string column_name;
        std::vector<char> query_varlena;  // full detoasted varlena bytes, 16-byte aligned by std::allocator
        uint8_t alphabet_code          = 0;
        uint8_t width_code             = 0;
        bool active                    = false;

        [[nodiscard]] const PgCompactSequenceHdr* query_hdr() const {
            if (!active || query_varlena.empty()) {
                return nullptr;
            }
            return reinterpret_cast<const PgCompactSequenceHdr*>(query_varlena.data());
        }
    };

    /*
     * One buffered scan tuple. The slot copy holds the full base-relation
     * tuple so projection can resolve any Var reference at read time.
     * primary_key is the detoasted INT4 value of the PK column; it is
     * used as the map key by the batch processor. seq_varlena is a
     * detoasted copy of the mmsearch target column's bioseq.seq value;
     * it is passed through BatchProcessor::process_sequences as the
     * per-row target sequence. Copying rather than pointing into the
     * slot is mandatory: tts_values[] is reused across scan calls and
     * the TOAST-detoasted buffer lives in CurrentMemoryContext which
     * may reset between rows.
     */
    struct BufferedScanEntry {
        TupleTableSlot* scan_slot_copy      = nullptr;
        int32_t         primary_key         = -1;
        bool            primary_key_is_null = true;
        int64_t         sequence_rowno      = -1;   /* derived from scan_slot->tts_tid; -1 when the TID was invalid */
        std::vector<char> seq_varlena;
        bool            seq_is_null         = true;

        [[nodiscard]] const PgCompactSequenceHdr* seq_hdr() const {
            if (seq_is_null || seq_varlena.empty()) {
                return nullptr;
            }
            return reinterpret_cast<const PgCompactSequenceHdr*>(seq_varlena.data());
        }
    };

    /*
     * A cursor into (buffered_entries, match_map). output_cursors is
     * built in insertion order so EXPLAIN output mirrors INSERT order.
     */
    struct OutputCursor {
        size_t scan_entry_index = 0;
        size_t match_index      = 0;
    };

    struct NextOutput {
        BufferedScanEntry* entry;
        const MatchResult* match;
    };

private:
    BatchProcessor processor;
    MmsearchContext mmsearch_context;

    std::vector<BufferedScanEntry> buffered_entries;
    std::map<int32_t, std::vector<MatchResult>> match_map;
    std::vector<OutputCursor> output_cursors;
    int output_position = -1;

public:
    explicit TupleDispatchBuffer(const size_t estimated_tuple_count) {
        buffered_entries.reserve(estimated_tuple_count * 2);
        output_cursors.reserve(estimated_tuple_count * 2);
    }

    /*
     * Record the mmsearch column / pk / query triple extracted from the
     * plan. Called once per scan from ColumnarScan_BeginCustomScan.
     * query_varlena_bytes owns the detoasted varlena — moved in.
     */
    void configure_mmsearch(AttrNumber seq_attno,
                            std::string column_name,
                            std::vector<char> query_varlena_bytes,
                            uint8_t alphabet_code,
                            uint8_t width_code,
                            AttrNumber primary_key_attno) {
        mmsearch_context.seq_attno          = seq_attno;
        mmsearch_context.primary_key_attno  = primary_key_attno;
        mmsearch_context.column_name        = std::move(column_name);
        mmsearch_context.query_varlena      = std::move(query_varlena_bytes);
        mmsearch_context.alphabet_code      = alphabet_code;
        mmsearch_context.width_code         = width_code;
        mmsearch_context.active             = true;

        uint32_t query_bp_size = 0;
        if (const auto* hdr = mmsearch_context.query_hdr()) {
            query_bp_size = hdr->bp_size;
        }
        JustLog("[bioseq] pg: TupleDispatchBuffer::configure_mmsearch() seq_attno={} pk_attno={} column='{}' typmod=({},{}) query_bp_size={}",
                static_cast<int>(seq_attno),
                static_cast<int>(primary_key_attno),
                mmsearch_context.column_name,
                static_cast<unsigned>(alphabet_code),
                static_cast<unsigned>(width_code),
                query_bp_size);
    }

    void clear_mmsearch_context() {
        mmsearch_context = MmsearchContext{};
    }

    /*
     * Writing-phase hook. Fired from Inner_CustomExecScan after ExecQual
     * passes, using econtext->ecxt_scantuple which still holds every
     * attribute of the base relation. Extracts the bioseq.seq column
     * named by configure_mmsearch (detoasting its varlena), the PK
     * column value, and copies the slot into a MinimalTuple-backed
     * clone so the data outlives the next ExecScanFetch call.
     */
    [[gnu::always_inline]] void submit_scan(TupleTableSlot* scan_slot) {
        if (!mmsearch_context.active) {
            JustLog("[bioseq] pg: submit_scan called while mmsearch_context inactive; skipping");
            return;
        }
        if (scan_slot == nullptr) {
            JustLog("[bioseq] pg: submit_scan received null scan_slot; skipping");
            return;
        }

        BufferedScanEntry entry{};

        /*
         * Capture the scan slot's tts_tid (set by the bioseq columnar
         * access method in ColumnarScanNext) and invert it back to a
         * row number so downstream consumers can join against the
         * sidecar tables (keyed on (user_relid, sequence_rowno)). Uses
         * the canonical tid_to_row_number helper from access_method.cppm
         * so the encoding stays in lockstep with row_number_to_tid.
         */
        if (ItemPointerIsValid(&scan_slot->tts_tid)) {
            entry.sequence_rowno = static_cast<int64_t>(
                tid_to_row_number(scan_slot->tts_tid));
        }

        /* Extract the target bioseq.seq column (mmsearch arg0). */
        if (mmsearch_context.seq_attno == InvalidAttrNumber) {
            entry.seq_is_null = true;
        } else {
            bool is_null = true;
            Datum seq_datum = slot_getattr(scan_slot, mmsearch_context.seq_attno, &is_null);
            if (is_null) {
                entry.seq_is_null = true;
            } else {
                /*
                 * PG_DETOAST_DATUM resolves short-header/TOASTed/compressed
                 * varlenas into a flat 4-byte-header copy we can reinterpret
                 * as PgCompactSequenceHdr*. The copy lives in
                 * CurrentMemoryContext, which is the per-query context here
                 * — it will be reset long after we finish writing, so the
                 * read below is safe. We still memcpy the bytes into our
                 * std::vector<char> so the BufferedScanEntry survives
                 * ResetExprContext / per-tuple context cleanup between
                 * calls to ExecScanFetch.
                 */
                PgCompactSequenceHdr* src_hdr = reinterpret_cast<PgCompactSequenceHdr*>(
                    PG_DETOAST_DATUM(seq_datum));
                const size_t total_bytes = static_cast<size_t>(VARSIZE(src_hdr));
                entry.seq_varlena.assign(
                    reinterpret_cast<const char*>(src_hdr),
                    reinterpret_cast<const char*>(src_hdr) + total_bytes);
                entry.seq_is_null = false;
                /*
                 * Free the detoasted copy if it was distinct from the
                 * original Datum (only the TOAST path actually palloc's).
                 */
                if (reinterpret_cast<void*>(src_hdr)
                    != reinterpret_cast<void*>(DatumGetPointer(seq_datum))) {
                    pfree(src_hdr);
                }
            }
        }

        /* Extract the primary-key column. */
        if (mmsearch_context.primary_key_attno == InvalidAttrNumber) {
            entry.primary_key_is_null = true;
        } else {
            bool pk_null = true;
            Datum pk_datum = slot_getattr(scan_slot, mmsearch_context.primary_key_attno, &pk_null);
            if (pk_null) {
                entry.primary_key_is_null = true;
            } else {
                entry.primary_key = DatumGetInt32(pk_datum);
                entry.primary_key_is_null = false;
            }
        }

        /*
         * Force materialization of every attribute before cloning. The
         * columnar scan only populates tts_values[] for attributes in
         * attrNeededList; anything outside that set carries a stale
         * Datum. ExecCopySlotMinimalTuple serialises tts_values[] to a
         * MinimalTuple, and slot_getallattrs() guarantees every entry
         * is valid before serialisation.
         */
        slot_getallattrs(scan_slot);

        entry.scan_slot_copy = MakeSingleTupleTableSlot(
            scan_slot->tts_tupleDescriptor, &TTSOpsMinimalTuple);
        ExecStoreMinimalTuple(ExecCopySlotMinimalTuple(scan_slot),
                              entry.scan_slot_copy, /*shouldFree=*/true);

        /*
         * Eagerly materialize all attributes of the copied slot into
         * tts_values[]/tts_isnull[]. Without this, a downstream
         * ExecBuildProjectionInfo + ExecProject sees tts_nvalid=0 and
         * still works via the deopt getsomeattrs callback — but during
         * debugging we observed cases where the path built against the
         * copied slot's tupdesc fell back to zero-valued reads. Forcing
         * materialization now removes that ambiguity for zero cost.
         */
        slot_getallattrs(entry.scan_slot_copy);

        const uint32_t entry_seq_bp_size = entry.seq_hdr() != nullptr
            ? entry.seq_hdr()->bp_size : 0;
        JustLog("[bioseq] pg: submit_scan pk={} rowno={} seq_bp_size={} (null={})",
                entry.primary_key_is_null ? -1 : entry.primary_key,
                entry.sequence_rowno,
                entry_seq_bp_size,
                entry.seq_is_null);

        buffered_entries.push_back(std::move(entry));
    }

    /*
     * Batch-phase transition. Called when the access method returns
     * TupIsNull. Invokes BatchProcessor::process_sequences on the
     * buffered target varlenas plus the stashed query varlena; stashes
     * the returned MatchResult map; builds output_cursors in insertion
     * order (one per MatchResult per row, skipping rows whose PK is
     * absent from the map). The query/target type dispatch happens
     * inside process_sequences via dispatch_pgseq.
     */
    void run_batch_and_build_cursors() {
        std::array<const PgCompactSequenceHdr*, kDispatchBatchCapacity> target_hdrs{};
        std::array<int32_t,                      kDispatchBatchCapacity> query_ids{};

        const size_t batch_size = buffered_entries.size() > kDispatchBatchCapacity
            ? kDispatchBatchCapacity
            : buffered_entries.size();
        if (buffered_entries.size() > kDispatchBatchCapacity) {
            JustLog("[bioseq] pg: TupleDispatchBuffer batch overflow {} > {}; tail truncated",
                    buffered_entries.size(), kDispatchBatchCapacity);
        }

        for (size_t i = 0; i < batch_size; ++i) {
            const BufferedScanEntry& entry = buffered_entries[i];
            target_hdrs[i] = entry.seq_hdr();  // nullptr for NULL seq
            query_ids[i]   = entry.primary_key_is_null ? -1 : entry.primary_key;
        }

        const PgCompactSequenceHdr* query_hdr = mmsearch_context.query_hdr();
        const uint32_t query_bp_size = query_hdr != nullptr ? query_hdr->bp_size : 0;
        JustLog("[bioseq] pg: TupleDispatchBuffer calling BatchProcessor::process_sequences batch_size={} typmod=({},{}) query_bp_size={}",
                batch_size,
                static_cast<unsigned>(mmsearch_context.alphabet_code),
                static_cast<unsigned>(mmsearch_context.width_code),
                query_bp_size);

        match_map = processor.template process_sequences<int32_t, kDispatchBatchCapacity>(
            batch_size,
            mmsearch_context.alphabet_code,
            mmsearch_context.width_code,
            query_hdr,
            target_hdrs, query_ids);

        output_cursors.clear();
        for (size_t i = 0; i < buffered_entries.size(); ++i) {
            const BufferedScanEntry& entry = buffered_entries[i];
            if (entry.primary_key_is_null) {
                JustLog("[bioseq] pg: batch skip: null primary key for entry {}", i);
                continue;
            }
            auto hit = match_map.find(entry.primary_key);
            if (hit == match_map.end()) {
                JustLog("[bioseq] pg: batch miss for PK={}; WHERE mmsearch evaluates to FALSE",
                        entry.primary_key);
                continue;
            }
            for (size_t m = 0; m < hit->second.size(); ++m) {
                output_cursors.push_back({ i, m });
            }
        }
        output_position = 0;
        JustLog("[bioseq] pg: batch produced {} output tuples from {} scan tuples",
                output_cursors.size(), buffered_entries.size());
    }

    /*
     * Reading-phase iterator. Returns nullopt when the cursor is drained
     * (signals EOF to the custom scan). Otherwise returns the entry and
     * the matched MatchResult.
     */
    [[nodiscard]] std::optional<NextOutput> read_next_output() {
        if (output_position < 0
            || static_cast<size_t>(output_position) >= output_cursors.size()) {
            return std::nullopt;
        }
        const OutputCursor& cursor = output_cursors[output_position];
        output_position++;
        if (cursor.scan_entry_index >= buffered_entries.size()) {
            JustLog("[bioseq] pg: cursor references missing scan entry {}", cursor.scan_entry_index);
            return std::nullopt;
        }
        BufferedScanEntry& entry = buffered_entries[cursor.scan_entry_index];
        auto hit = match_map.find(entry.primary_key);
        if (hit == match_map.end() || cursor.match_index >= hit->second.size()) {
            JustLog("[bioseq] pg: cursor references missing match for PK={}", entry.primary_key);
            return std::nullopt;
        }
        return NextOutput{ &entry, &hit->second[cursor.match_index] };
    }

    /*
     * True once run_batch_and_build_cursors has been called for the
     * current scan. Inner_CustomExecScan uses this to switch from the
     * writing-phase access-method loop to the reading-phase drain loop.
     */
    [[nodiscard]] constexpr bool has_finished_writing() const {
        return output_position >= 0;
    }

    /*
     * True when BeginCustomScan found a WHERE mmsearch(...) qual and
     * installed its (seq_attno, query_varlena, typmod) context. When
     * false the custom scan must behave like a pass-through scan:
     * fetch rows, project them directly via the (VarInjectorMutator-
     * rewritten) target list, and return them without buffering. The
     * buffering + batch pipeline is only meaningful when there is a
     * WHERE mmsearch(...) to drive it. See SPEC 2 §6.8 for the
     * historical gap this accessor closes.
     */
    [[nodiscard]] constexpr bool mmsearch_active() const {
        return mmsearch_context.active;
    }

    [[nodiscard]] size_t buffered_count() const {
        return buffered_entries.size();
    }

    [[nodiscard]] size_t output_count() const {
        return output_cursors.size();
    }

    /*
     * Reset writing/reading state so a new scan can start. Frees the
     * MinimalTuple slots allocated in submit_scan; leaves
     * mmsearch_context untouched so callers can reuse configuration.
     */
    [[gnu::always_inline]] void clear_slots() {
        JustLog("[bioseq] pg: TupleDispatchBuffer::clear_slots() entered; freeing {} slots, {} outputs",
                buffered_entries.size(), output_cursors.size());
        for (auto& entry : buffered_entries) {
            if (entry.scan_slot_copy != nullptr) {
                ExecDropSingleTupleTableSlot(entry.scan_slot_copy);
                entry.scan_slot_copy = nullptr;
            }
        }
        buffered_entries.clear();
        match_map.clear();
        output_cursors.clear();
        output_position = -1;
    }

    [[gnu::always_inline]] void clear() {
        clear_slots();
        clear_mmsearch_context();
    }
};

} // namespace bioseq::simd::HWY_NAMESPACE
HWY_AFTER_NAMESPACE();
