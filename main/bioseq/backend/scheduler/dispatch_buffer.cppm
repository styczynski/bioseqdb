/*-------------------------------------------------------------------------
 *
 * scheduler/dispatch_buffer.cppm
 *
 * TupleDispatchBuffer accumulates TupleTableSlot* tuples produced during
 * a custom-scan execution phase so the scan node can later replay them in
 * order. The scan first runs in "writing" mode (calls to submit()), then
 * transitions to "reading" mode by calling finish_writing(); subsequent
 * calls to read_next() drain the buffer one slot at a time, returning
 * nullptr once the buffer is exhausted.
 *
 * on_submit() is a pre-projection hook the custom scan fires for every
 * tuple that passed the WHERE qual. It reads a TEXT column captured at
 * BeginCustomScan time (configured via configure_mmsearch) and emits a
 * JustLog line describing the would-be mmsearch call. Later the same
 * hook will drive the real aligner.
 *
 *-------------------------------------------------------------------------
 */
module;

#include "common.h"

#include <string>
#include <vector>

export module bioseq_scheduler.dispatch_buffer;

import mlogging;
import pg_utils;

extern "C++" {

export class TupleDispatchBuffer {
public:
    /*
     * Context captured from cscan->scan.plan.qual at BeginCustomScan
     * time. column_attno is the 1-based attribute number of the Var
     * passed as the first argument of mmsearch(); query_string is the
     * TEXT Const passed as the second argument. Both are owned by
     * std::string so they survive across scans and don't dangle when
     * the surrounding PG MemoryContext is reset.
     */
    struct MmsearchContext {
        AttrNumber column_attno = InvalidAttrNumber;
        std::string column_name;
        std::string query_string;
        bool active = false;
    };

private:
    std::vector<TupleTableSlot*> buffered_slots;
    int buffer_position = -1;
    MmsearchContext mmsearch_context;

public:
    explicit TupleDispatchBuffer(const size_t estimated_tuple_count) {
        buffered_slots.reserve(estimated_tuple_count * 2);
    }

    /*
     * Record the column/query pair extracted from the WHERE mmsearch
     * call. Called once per scan from ColumnarScan_BeginCustomScan.
     * Overwrites any previous context.
     */
    void configure_mmsearch(AttrNumber attno, std::string column_name, std::string query_string) {
        mmsearch_context.column_attno = attno;
        mmsearch_context.column_name = std::move(column_name);
        mmsearch_context.query_string = std::move(query_string);
        mmsearch_context.active = true;
        JustLog("[bioseq] pg: TupleDispatchBuffer::configure_mmsearch() attno={} column='{}' query='{}'",
                static_cast<int>(attno),
                mmsearch_context.column_name,
                mmsearch_context.query_string);
    }

    void clear_mmsearch_context() {
        mmsearch_context.column_attno = InvalidAttrNumber;
        mmsearch_context.column_name.clear();
        mmsearch_context.query_string.clear();
        mmsearch_context.active = false;
    }

    /*
     * Hook fired for every source tuple that has passed the WHERE qual,
     * BEFORE the tuple is projected. scan_slot is the scan-side slot
     * that still carries every column of the base relation. Extracts
     * the TEXT-typed column identified by configure_mmsearch() and
     * emits the expected log line.
     *
     * Does nothing if configure_mmsearch() has not been called for
     * this scan, or if the configured attribute is NULL for this row.
     */
    [[gnu::always_inline]] void on_submit(TupleTableSlot* scan_slot) {
        if (!mmsearch_context.active) {
            return;
        }
        if (mmsearch_context.column_attno == InvalidAttrNumber) {
            return;
        }
        if (scan_slot == nullptr) {
            JustLog("[bioseq] pg: TupleDispatchBuffer::on_submit() scan_slot is null -- skipping");
            return;
        }
        bool is_null = true;
        Datum column_datum = slot_getattr(scan_slot, mmsearch_context.column_attno, &is_null);
        if (is_null) {
            JustLog("need to do mmsearch on column value NULL and provided argument string '{}'",
                    mmsearch_context.query_string);
            return;
        }
        char* column_cstring = TextDatumGetCString(column_datum);
        JustLog("need to do mmsearch on column value '{}' and provided argument string '{}'",
                column_cstring,
                mmsearch_context.query_string);
        pfree(column_cstring);
    }

    /*
     * Copies the supplied slot into a freshly allocated MinimalTuple slot
     * and appends it to the buffer.
     */
    [[gnu::always_inline]] void submit(TupleTableSlot* source_slot) {
        TupleTableSlot* copied_slot = MakeSingleTupleTableSlot(source_slot->tts_tupleDescriptor, &TTSOpsMinimalTuple);
        ExecStoreMinimalTuple(ExecCopySlotMinimalTuple(source_slot), copied_slot, true);
        buffered_slots.push_back(copied_slot);
    }

    /*
     * Resets only the buffered slot vector. Used by call sites that
     * want to start a fresh replay window while keeping the mmsearch
     * context configured by BeginCustomScan.
     */
    [[gnu::always_inline]] void clear_slots() {
        JustLog("[bioseq] pg: TupleDispatchBuffer::clear_slots() entered");
        buffered_slots.clear();
        buffer_position = -1;
    }

    /*
     * Resets both the slot buffer and the mmsearch context. Kept for
     * callers that want a fully clean state.
     */
    [[gnu::always_inline]] void clear() {
        clear_slots();
        clear_mmsearch_context();
    }

    /*
     * Transitions the buffer from writing to reading. Returns the first
     * buffered slot (or nullptr when the buffer is empty) and advances
     * the read cursor.
     */
    [[gnu::always_inline]] [[nodiscard]] TupleTableSlot* finish_writing() {
        buffer_position++;
        if (static_cast<size_t>(buffer_position) < buffered_slots.size()) {
            return buffered_slots[buffer_position++];
        }
        return nullptr;
    }

    /*
     * True after finish_writing() has been called and the buffer has at
     * least one consumed entry.
     */
    [[gnu::always_inline]] [[nodiscard]] constexpr bool has_finished_writing() const {
        return buffer_position >= 0;
    }

    /*
     * Returns the next buffered slot or nullptr when the buffer is
     * exhausted. Always pair calls to read_next() with a prior
     * finish_writing() that returned non-null.
     */
    [[gnu::always_inline]] [[nodiscard]] TupleTableSlot* read_next() {
        JustLog("[bioseq] pg: TupleDispatchBuffer::read_next() position={} size={}", buffer_position, buffered_slots.size());
        if (static_cast<size_t>(buffer_position) < buffered_slots.size()) {
            TupleTableSlot* current_slot = buffered_slots[buffer_position];
            buffer_position++;
            return current_slot;
        }
        return nullptr;
    }
};

}
