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
 *   records the row's primary key and its `seq` TEXT value. Projection
 *   does NOT happen here; `ecxt_scantuple` still references the original
 *   scan slot which is about to be overwritten by the next call.
 *
 *   Batch phase (run_batch_and_build_cursors): after the access method
 *   signals EOF, the buffer feeds its `seq`/pk arrays into
 *   BatchProcessor::process_sequences and records the returned
 *   MatchResult map. One OutputCursor is appended per MatchResult
 *   returned for a given primary key. Rows whose PK is missing from the
 *   map are skipped; this is how WHERE mmsearch(...) evaluates to
 *   FALSE for those rows.
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
#include <vector>

export module bioseq_scheduler.dispatch_buffer;

import mlogging;
import pg_utils;
import bioseq_sequences.minimap_port;
import bioseq_cigar;

HWY_BEFORE_NAMESPACE();

namespace bioseq::simd::HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

// Fixed upper bound on a single batch. Matches the TupleDispatchBuffer
// constructor hint; entries beyond this get truncated with a log line.
constexpr size_t kDispatchBatchCapacity = 1000;

export class BatchProcessor {
public:
    // This should be called for batch of column inputs when finish_writing is called
    template<typename K, size_t batch_capacity>
    [[always_inline]] constexpr std::map<K, std::vector<MatchResult>> process_sequences(
        const size_t batch_size, //< number of queries in the batch
        const char* target_sequence, //< sequence passed in mmsearch_context.query_string
        const std::array<const char*, batch_capacity>& query_sequences, //< should be batch of column strings. Note that batch_capacity can be higher than actual number of queries. Use batch_size to pass actual number of queries in batch
        const std::array<K, batch_capacity>& query_ids //< we need to pass some row identitier (please extract primary key of the table to use here)
    ) {
        // Some dummy implementation
        std::map<K, std::vector<MatchResult>> results;
        // We skip first sequence to test what happens if the sequence is discarded
        // Note that this function can return partial map without all input queries.
        // If query is not in the returned map then it should be interpreted as "where mmsearch(seq, 'sequence_b')" returning FALSE (so the tuple should be completely filtered out from results)
        for (size_t query_no=1; query_no<batch_size; ++query_no) {
            std::vector<CigarOp> cigar = {
                {
                    .length = 5,
                    .opcode = CIGAR_DEL
                },
                {
                    .length = 3,
                    .opcode = CIGAR_MATCH
                },
                {
                    .length = 8,
                    .opcode = CIGAR_EQ
                },
                {
                    .length = 1,
                    .opcode = CIGAR_INS
                }
            };
            const MatchResult match = {
                .query_len = static_cast<int32_t>(query_no*2),    ///< col 2: query sequence length
                .query_start            = static_cast<int32_t>(query_no+1),    ///< col 3: query start (0-based, closed)
                .query_end              = static_cast<int32_t>(query_no*3+5),    ///< col 4: query end (0-based, open)
                .is_reverse             = query_no % 2 == 0,///< col 5: '+' or '-'
                .target_len             = static_cast<int32_t>(query_no+2),    ///< col 7: target sequence length
                .target_start           = static_cast<int32_t>(query_no*4+7),    ///< col 8: target start on original strand
                .target_end             = static_cast<int32_t>(query_no*5+9),    ///< col 9: target end on original strand
                .num_matches            = static_cast<int32_t>(query_no),    ///< col 10: number of residue matches (mlen)
                .block_length           = static_cast<int32_t>(query_no),    ///< col 11: alignment block length (blen)
                .mapq                   = static_cast<int32_t>(query_no),    ///< col 12: mapping quality (0-255)
                .chain_score            = static_cast<int32_t>(query_no),    ///< chaining score
                .dp_score               = static_cast<int32_t>(query_no),    ///< DP alignment score
                .dp_max_score           = static_cast<int32_t>(query_no),    ///< max scoring segment score (dp_max0 in original)
                .seed_count             = static_cast<int32_t>(query_no),    ///< number of seeds in chain
                .secondary_chain_score  = static_cast<int32_t>(query_no),    ///< best secondary chaining score against this primary
                .divergence             = -1.f, ///< sequence divergence estimate
                .is_primary             = query_no % 3 == 0, ///< primary mapping flag
                .has_cigar              = true, ///< whether CIGAR alignment was performed
                .cigar                  = cigar
            };
            results[query_ids[query_no]].push_back(std::move(match));
        }
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
     * any bioseq.<field>(...) projection function. query_string is the
     * TEXT literal passed as the second argument of mmsearch(). Strings
     * are std::string-owned so they survive PG MemoryContext resets.
     */
    struct MmsearchContext {
        AttrNumber seq_attno           = InvalidAttrNumber;
        AttrNumber primary_key_attno   = InvalidAttrNumber;
        std::string column_name;
        std::string query_string;
        bool active = false;
    };

    /*
     * One buffered scan tuple. The slot copy holds the full base-relation
     * tuple so projection can resolve any Var reference at read time.
     * primary_key is the detoasted INT4 value of the PK column; it is
     * used as the map key by the batch processor. seq_string is the
     * detoasted TEXT value of the mmsearch target column, passed into
     * BatchProcessor::process_sequences as the per-row query.
     */
    struct BufferedScanEntry {
        TupleTableSlot* scan_slot_copy    = nullptr;
        int32_t         primary_key       = -1;
        bool            primary_key_is_null = true;
        std::string     seq_string;
        bool            seq_is_null       = true;
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
     * Record the mmsearch column/pk/query triple extracted from the
     * plan. Called once per scan from ColumnarScan_BeginCustomScan.
     */
    void configure_mmsearch(AttrNumber seq_attno,
                            std::string column_name,
                            std::string query_string,
                            AttrNumber primary_key_attno) {
        mmsearch_context.seq_attno          = seq_attno;
        mmsearch_context.primary_key_attno  = primary_key_attno;
        mmsearch_context.column_name        = std::move(column_name);
        mmsearch_context.query_string       = std::move(query_string);
        mmsearch_context.active             = true;
        JustLog("[bioseq] pg: TupleDispatchBuffer::configure_mmsearch() seq_attno={} pk_attno={} column='{}' query='{}'",
                static_cast<int>(seq_attno),
                static_cast<int>(primary_key_attno),
                mmsearch_context.column_name,
                mmsearch_context.query_string);
    }

    void clear_mmsearch_context() {
        mmsearch_context = MmsearchContext{};
    }

    /*
     * Writing-phase hook. Fired from Inner_CustomExecScan after ExecQual
     * passes, using econtext->ecxt_scantuple which still holds every
     * attribute of the base relation. Extracts the TEXT column named by
     * configure_mmsearch, the PK column value, and copies the slot into
     * a MinimalTuple-backed clone so the data outlives the next
     * ExecScanFetch call.
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

        /* Extract the target TEXT column (mmsearch arg0). */
        if (mmsearch_context.seq_attno == InvalidAttrNumber) {
            entry.seq_is_null = true;
        } else {
            bool is_null = true;
            Datum seq_datum = slot_getattr(scan_slot, mmsearch_context.seq_attno, &is_null);
            if (is_null) {
                entry.seq_is_null = true;
            } else {
                char* cstr = TextDatumGetCString(seq_datum);
                entry.seq_string = cstr;
                pfree(cstr);
                entry.seq_is_null = false;
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

        JustLog("need to do mmsearch on column value '{}' and provided argument string '{}'",
                entry.seq_is_null ? std::string("NULL") : entry.seq_string,
                mmsearch_context.query_string);

        buffered_entries.push_back(std::move(entry));
    }

    /*
     * Batch-phase transition. Called when the access method returns
     * TupIsNull. Invokes BatchProcessor::process_sequences on the
     * buffered inputs, stashes the returned MatchResult map, and builds
     * output_cursors in insertion order (one per MatchResult per row,
     * skipping rows whose PK is absent from the map).
     */
    void run_batch_and_build_cursors() {
        std::array<const char*, kDispatchBatchCapacity> query_sequences{};
        std::array<int32_t,     kDispatchBatchCapacity> query_ids{};

        const size_t batch_size = buffered_entries.size() > kDispatchBatchCapacity
            ? kDispatchBatchCapacity
            : buffered_entries.size();
        if (buffered_entries.size() > kDispatchBatchCapacity) {
            JustLog("[bioseq] pg: TupleDispatchBuffer batch overflow {} > {}; tail truncated",
                    buffered_entries.size(), kDispatchBatchCapacity);
        }

        for (size_t i = 0; i < batch_size; ++i) {
            const BufferedScanEntry& entry = buffered_entries[i];
            query_sequences[i] = entry.seq_is_null ? "" : entry.seq_string.c_str();
            query_ids[i]       = entry.primary_key_is_null ? -1 : entry.primary_key;
        }

        const char* target = mmsearch_context.query_string.c_str();

        JustLog("[bioseq] pg: TupleDispatchBuffer calling BatchProcessor::process_sequences batch_size={} target='{}'",
                batch_size, target);

        match_map = processor.template process_sequences<int32_t, kDispatchBatchCapacity>(
            batch_size, target, query_sequences, query_ids);

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
