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
import bioseq_sequences.kmers;
import bioseq_sequences.kmers.hashers.minimap_int;
import bioseq_sequences.minimap_port;
import bioseq_cigar;
import bioseq_types.pg_compact_sequence;
import bioseq_sidecar;

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
/*
 * Execution counters captured while BatchProcessor walks a batch. Used by
 * EXPLAIN to print an accurate "Recomputed minimizers: X%" estimate.
 * The struct lives next to the BatchProcessor instance inside the
 * TupleDispatchBuffer so EXPLAIN can read it after run_batch_and_build_cursors
 * without doing a second walk.
 */
export struct BatchExecutionStats {
    /// Total rows submitted to process_sequences().
    size_t targets_considered = 0;
    /// Rows that arrived with a prebuilt index pointer and reused it.
    size_t targets_with_prebuilt_index = 0;
    /// Rows whose minimizers had to be recomputed from the compact_sequence_range.
    size_t targets_recomputed_minimizers = 0;
    /// Effective parameters used for the last batch. Captured for EXPLAIN so
    /// the user sees the actually-applied values after overrides + size-heuristic
    /// adjustments.
    Minimap2Params last_effective_params{};

    [[nodiscard]] float recomputed_fraction() const {
        return targets_considered == 0
            ? 0.0f
            : static_cast<float>(targets_recomputed_minimizers)
              / static_cast<float>(targets_considered);
    }
};

/*
 * Scale k/w for the small-query regime typical inside Postgres — short
 * motifs vs per-row sequences under 1 kb. Runs once per row before the
 * core pipeline. Factored out of the hot lambda so all code paths
 * (recompute vs prebuilt index) share a single size-heuristic knob and
 * so EXPLAIN can replay the same derivation for its param printout.
 *
 * The heuristic only fires when the user left the fields at their
 * minimap2 defaults. Any aligner_options() override or non-default GUC
 * value short-circuits the adjustment so we never silently overwrite a
 * user-supplied tuning value.
 */
[[nodiscard]] inline Minimap2Params apply_size_heuristic(
    const Minimap2Params& base, size_t min_bp)
{
    /*
     * Only scale when every relevant parameter still matches the minimap2
     * default. If the caller has already tuned one of these fields, we
     * trust them and leave the batch alone.
     */
    const bool all_defaults =
        base.kmer_size       == 15 &&
        base.window_size     == 10 &&
        base.min_chain_score == 40 &&
        base.min_seed_count  == 3;
    if (!all_defaults) {
        return base;
    }
    Minimap2Params scaled = base;
    if (min_bp < 50) {
        scaled.kmer_size       = 8;
        scaled.window_size     = 3;
        scaled.min_chain_score = 10;
        scaled.min_seed_count  = 1;
    } else if (min_bp < 200) {
        scaled.kmer_size       = 10;
        scaled.window_size     = 5;
        scaled.min_chain_score = 20;
        scaled.min_seed_count  = 2;
    }
    return scaled;
}

export class BatchProcessor {
public:
    /*
     * Per-row sidecar blob pointer + length pair. The pointer references
     * memory owned by the caller's BufferedScanEntry (its own
     * sidecar_blob std::vector<char>) — we never copy the bytes into the
     * BatchProcessor. A null pointer or zero length means "no prebuilt
     * index, recompute minimizers for this row".
     */
    struct TargetIndexBlobRef {
        const char* data  = nullptr;
        std::size_t bytes = 0;
    };

    /*
     * Run the minimap2 pipeline across a batch. index_blobs may be nullptr
     * when the caller has no sidecar path (legacy/read_sequence_index=off);
     * otherwise it points at a contiguous block of batch_capacity pairs,
     * one per target. Rows with an empty blob fall back to the recompute
     * path so the caller can mix "index present" and "index absent" rows
     * in the same scan (the common case when update_index_on_insert was
     * toggled during loading).
     */
    /*
     * NOTE on parameter passing: `target_hdrs`, `query_ids`, and
     * `index_blobs` are full-size `kDispatchBatchCapacity`-element arrays
     * (8 KB, 4 KB, and 16 KB respectively on a typical batch). They are
     * all passed by const-ref / pointer so the call does not copy ~28 KB
     * into the callee. The result map is RVO'd / moved on return.
     */
    template<typename K, size_t batch_capacity>
    [[nodiscard]] std::map<K, std::vector<MatchResult>> process_sequences(
        const size_t batch_size,
        const uint8_t alphabet_code,
        const uint8_t width_code,
        const PgCompactSequenceHdr* query_hdr,
        const std::array<const PgCompactSequenceHdr*, batch_capacity>& target_hdrs,
        const std::array<K, batch_capacity>& query_ids,
        const Minimap2Params& base_params,
        const std::array<TargetIndexBlobRef, batch_capacity>* index_blobs,
        BatchExecutionStats& stats)
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

                /*
                 * Query minimizers are computed once per batch for the
                 * prebuilt-index path (every row reuses the same query).
                 * Recompute-path rows use minimap_port(), which does its
                 * own sketching; the extra allocation here is avoided
                 * for those rows via the optional nested-scope.
                 */
                std::optional<std::vector<MinimapSketchData>> cached_query_minimizers;

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

                    const size_t min_bp = std::min(query_seq.bp_size(),
                                                   target_seq.bp_size());

                    ++stats.targets_considered;

                    /*
                     * Prebuilt-index fast path — if the custom scan
                     * captured a serialized TargetIndex blob for this
                     * row, deserialize it in place and call
                     * minimap2_port_with_index so we skip the target
                     * sketch pass. The deserialized TargetIndex owns
                     * its unordered_map buckets and dies at scope exit.
                     *
                     * CRITICAL: the sidecar index was built with the
                     * w=10/k=15 defaults baked into
                     * sidecar_compute_and_write_index (see sidecar.cppm).
                     * Query minimizers extracted with different w/k
                     * values would hash to incompatible minimizers and
                     * miss every seed — so for the prebuilt-index
                     * branch we suppress the batch-level size heuristic
                     * (which otherwise lowers k/w for short targets).
                     * The recompute branch keeps the heuristic because
                     * both query and target are sketched under the same
                     * k/w there.
                     */
                    const bool has_blob =
                        index_blobs != nullptr
                        && (*index_blobs)[i].data != nullptr
                        && (*index_blobs)[i].bytes > 0;

                    MatchResultCollection collection;
                    if (has_blob) {
                        Minimap2Params params = base_params;
                        /* Pin the sketch parameters to the sidecar's k/w.
                         * Keep every other knob from base_params (bandwidth,
                         * scoring, algorithm selection, …) intact. */
                        params.kmer_size   = 15;
                        params.window_size = 10;
                        /*
                         * For very short inputs minimap2 defaults want
                         * min_chain_score=40/min_seed_count=3 which would
                         * reject every chain. Keep the rule-of-thumb
                         * thresholds from the heuristic, just without the
                         * k/w rewrite.
                         */
                        if (min_bp < 50) {
                            params.min_chain_score = 10;
                            params.min_seed_count  = 1;
                        } else if (min_bp < 200) {
                            params.min_chain_score = 20;
                            params.min_seed_count  = 2;
                        }

                        ++stats.targets_with_prebuilt_index;
                        if (!cached_query_minimizers.has_value()) {
                            /*
                             * Build query minimizers once per batch using
                             * the sidecar's pinned k/w. Every row in the
                             * prebuilt-index branch reuses this set.
                             */
                            sketch_collector query_collector;
                            {
                                EASY_BLOCK("mm_sketch");
                                collect_minimizers<minimap_int_hasher>(
                                    query_collector, query_seq,
                                    query_seq.begin(), query_seq.end(),
                                    static_cast<size_t>(params.window_size),
                                    static_cast<size_t>(params.kmer_size));
                            }
                            cached_query_minimizers.emplace(
                                query_collector.extract_sketches_data());
                        }
                        TargetIndex prebuilt_idx = TargetIndex::deserialize_from_binary(
                            (*index_blobs)[i].data, (*index_blobs)[i].bytes);
                        collection = minimap2_port_with_index(
                            query_seq, target_seq,
                            *cached_query_minimizers,
                            prebuilt_idx, params);
                        stats.last_effective_params = params;
                    } else {
                        ++stats.targets_recomputed_minimizers;
                        Minimap2Params params = apply_size_heuristic(base_params, min_bp);
                        collection = minimap_port(query_seq, target_seq, params);
                        stats.last_effective_params = params;
                    }

                    JustLog("[bioseq] pg: BatchProcessor row query_id={} target_bp_size={} k={} w={} min_chain_score={} min_seed_count={} prebuilt_index={} produced {} match(es)",
                            static_cast<int>(query_ids[i]),
                            target_seq.bp_size(),
                            stats.last_effective_params.kmer_size,
                            stats.last_effective_params.window_size,
                            stats.last_effective_params.min_chain_score,
                            stats.last_effective_params.min_seed_count,
                            has_blob, collection.size());
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

        /*
         * Per-row serialized TargetIndex bytes read from
         * bioseq_internal.index_sidecar. Populated by submit_scan only when
         * bioseq.read_sequence_index = on AND the lookup returned a non-empty
         * blob. When empty the batch processor falls back to recomputing
         * minimizers for this row. The std::vector<char> owns its bytes so
         * the BatchProcessor::TargetIndexBlobRef points directly into the
         * entry's storage without copying.
         */
        std::vector<char> sidecar_index_blob;

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

    /*
     * Per-scan base Minimap2Params — captured by ColumnarScan_BeginCustomScan
     * from the plan (GUC snapshot + per-call aligner_options() overrides) and
     * pushed into BatchProcessor::process_sequences once per batch. Every
     * row inherits these values with size-heuristic tuning applied if the
     * user left the minimap2 defaults in place.
     */
    Minimap2Params base_params{};

    /*
     * Controls whether submit_scan will attempt an index_sidecar lookup.
     * Captured at configure time so the submit_scan hot path doesn't touch
     * the GUC. The caller (custom scan) is expected to keep this value in
     * sync with bioseq.read_sequence_index.
     */
    bool read_sequence_index_active = false;

    /*
     * The OID of the relation being scanned. Only used to key into
     * bioseq_internal.index_sidecar; set at configure time so submit_scan
     * doesn't need to re-read RelationGetRelid per tuple.
     */
    Oid user_relid = InvalidOid;

    /*
     * Execution statistics captured during the last batch. Used by
     * ColumnarScan_ExplainCustomScan so it can compute the "Bioseq
     * Recomputed" percentage from runtime truth instead of the
     * plan-time estimate.
     */
    BatchExecutionStats last_stats{};

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
     * Install the per-scan aligner parameters. Called from
     * ColumnarScan_BeginCustomScan after the GUC snapshot + any per-call
     * aligner_options() overrides have been applied. Must be called before
     * the first submit_scan.
     */
    void configure_params(const Minimap2Params& params,
                          bool read_sequence_index,
                          Oid scan_user_relid) {
        base_params                  = params;
        read_sequence_index_active   = read_sequence_index;
        user_relid                   = scan_user_relid;
        last_stats                   = BatchExecutionStats{};
    }

    [[nodiscard]] const Minimap2Params& base_aligner_params() const { return base_params; }
    [[nodiscard]] const BatchExecutionStats& execution_stats() const { return last_stats; }
    [[nodiscard]] bool reads_sequence_index() const { return read_sequence_index_active; }

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

        /*
         * Sidecar index read — only when the GUC is on, the row has a
         * stable rowno, and there is a seq varlena (nothing to do for
         * NULL-seq rows). We intentionally keep this INSIDE submit_scan
         * so it happens once per row at buffering time; doing it later
         * would require re-opening relcache entries per batch row.
         *
         * sidecar_read_index returns an owned std::vector<char>; moving
         * it into entry.sidecar_index_blob avoids a memcpy. When the row
         * has no stored blob (update_index_on_insert was off) the
         * vector stays empty and the batch processor falls back to
         * recomputing minimizers — that fallback is counted toward the
         * EXPLAIN "recomputed" percentage.
         */
        if (read_sequence_index_active
            && !entry.seq_is_null
            && entry.sequence_rowno >= 0
            && OidIsValid(user_relid)) {
            auto blob = bioseq::sidecar::sidecar_read_index(
                user_relid, entry.sequence_rowno);
            if (blob.has_value()) {
                entry.sidecar_index_blob = std::move(*blob);
            }
        }

        const uint32_t entry_seq_bp_size = entry.seq_hdr() != nullptr
            ? entry.seq_hdr()->bp_size : 0;
        JustLog("[bioseq] pg: submit_scan pk={} rowno={} seq_bp_size={} (null={}) sidecar_blob_bytes={}",
                entry.primary_key_is_null ? -1 : entry.primary_key,
                entry.sequence_rowno,
                entry_seq_bp_size,
                entry.seq_is_null,
                entry.sidecar_index_blob.size());

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
        std::array<typename BatchProcessor::TargetIndexBlobRef,
                   kDispatchBatchCapacity> index_blobs{};

        const size_t batch_size = buffered_entries.size() > kDispatchBatchCapacity
            ? kDispatchBatchCapacity
            : buffered_entries.size();
        if (buffered_entries.size() > kDispatchBatchCapacity) {
            JustLog("[bioseq] pg: TupleDispatchBuffer batch overflow {} > {}; tail truncated",
                    buffered_entries.size(), kDispatchBatchCapacity);
        }

        /*
         * Populate the per-row blob reference array. The BatchProcessor
         * reads directly from these pointers into BufferedScanEntry's
         * own vector<char> storage — the vector keeps the bytes alive
         * for the full batch and no extra copy happens. Rows without a
         * sidecar blob stay at the default-constructed {nullptr, 0}
         * pair, which the batch processor interprets as "recompute".
         */
        for (size_t i = 0; i < batch_size; ++i) {
            const BufferedScanEntry& entry = buffered_entries[i];
            target_hdrs[i] = entry.seq_hdr();  // nullptr for NULL seq
            query_ids[i]   = entry.primary_key_is_null ? -1 : entry.primary_key;
            if (!entry.sidecar_index_blob.empty()) {
                index_blobs[i].data  = entry.sidecar_index_blob.data();
                index_blobs[i].bytes = entry.sidecar_index_blob.size();
            }
        }

        const PgCompactSequenceHdr* query_hdr = mmsearch_context.query_hdr();
        const uint32_t query_bp_size = query_hdr != nullptr ? query_hdr->bp_size : 0;
        JustLog("[bioseq] pg: TupleDispatchBuffer calling BatchProcessor::process_sequences batch_size={} typmod=({},{}) query_bp_size={} read_index={}",
                batch_size,
                static_cast<unsigned>(mmsearch_context.alphabet_code),
                static_cast<unsigned>(mmsearch_context.width_code),
                query_bp_size,
                read_sequence_index_active);

        last_stats = BatchExecutionStats{};
        const std::array<typename BatchProcessor::TargetIndexBlobRef,
                         kDispatchBatchCapacity>* blob_arg =
            read_sequence_index_active ? &index_blobs : nullptr;

        match_map = processor.template process_sequences<int32_t, kDispatchBatchCapacity>(
            batch_size,
            mmsearch_context.alphabet_code,
            mmsearch_context.width_code,
            query_hdr,
            target_hdrs, query_ids,
            base_params,
            blob_arg,
            last_stats);

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
