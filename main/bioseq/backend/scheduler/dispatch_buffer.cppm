/*-------------------------------------------------------------------------
 *
 * scheduler/dispatch_buffer.cppm
 *
 * TupleDispatchBuffer accumulates TupleTableSlot* tuples produced during a
 * custom-scan execution phase so the scan node can later replay them in
 * order. The scan first runs in "writing" mode (calls to submit()), then
 * transitions to "reading" mode by calling finish_writing(); subsequent
 * calls to read_next() drain the buffer one slot at a time, returning
 * nullptr once the buffer is exhausted.
 *
 * The submit() implementation copies each slot into a freshly allocated
 * MinimalTuple-backed slot so the buffered tuples remain valid past the
 * lifetime of the source slot.
 *
 *-------------------------------------------------------------------------
 */
module;

#include "common.h"

#include <vector>

export module bioseq_scheduler.dispatch_buffer;

import mlogging;

extern "C++" {

/*
 * Accumulator of TupleTableSlot* tuples. The two-phase API (submit() while
 * writing, finish_writing() to transition, then read_next() while reading)
 * is dictated by the surrounding custom-scan flow, which fills the buffer
 * during the scan loop and replays it once the source is exhausted.
 *
 * The buffer_position cursor uses a signed sentinel (-1) to mark the
 * "still writing" state so read_next() can be called repeatedly without
 * producing a false hit when the buffer is empty.
 */
export class TupleDispatchBuffer {
private:
    std::vector<TupleTableSlot*> buffered_slots;
    int buffer_position = -1;

public:
    /*
     * Pre-reserves storage for the expected number of tuples (doubled to
     * absorb growth bursts). Uses the supplied estimate as a hint only;
     * the buffer can grow beyond it.
     */
    explicit TupleDispatchBuffer(const size_t estimated_tuple_count) {
        buffered_slots.reserve(estimated_tuple_count * 2);
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
     * Resets both the contents and the read cursor. Call before reusing the
     * buffer for a new scan.
     */
    [[gnu::always_inline]] void clear() {
        JustLog("[bioseq] pg: TupleDispatchBuffer::clear() entered");
        buffered_slots.clear();
        buffer_position = -1;
    }

    /*
     * Transitions the buffer from writing to reading. Returns the first
     * buffered slot (or nullptr when the buffer is empty) and advances the
     * read cursor.
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
     * least one consumed entry. Used by callers to decide whether to keep
     * fetching from the source or drain the buffer.
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
