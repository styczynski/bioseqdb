/* Thin C-linkage fmgr entrypoints.  V1 records are registered centrally by
 * pg/mod.cppm so this partition contains no registration side effects. */
module;

#include <new>

#include "headers/pg_compat.h"

export module bioseq.types.pg_compact_sequence:sql_entrypoints;

import :io;
import :content;
import :letter;
import :subscript;
import :concat_reverse;
import :array_string;

#define BIOSEQ_SEQ_FORWARD(symbol, implementation)                         \
    extern "C" PGDLLEXPORT Datum symbol(PG_FUNCTION_ARGS)                 \
    {                                                                      \
        return implementation(fcinfo);                                     \
    }

BIOSEQ_SEQ_FORWARD(pg_compact_sequence_typmod_in, seq_typmod_in_impl)
BIOSEQ_SEQ_FORWARD(pg_compact_sequence_typmod_out, seq_typmod_out_impl)
BIOSEQ_SEQ_FORWARD(pg_compact_sequence_in, seq_in_impl)
BIOSEQ_SEQ_FORWARD(pg_compact_sequence_out, seq_out_impl)
BIOSEQ_SEQ_FORWARD(pg_compact_sequence_recv, seq_recv_impl)
BIOSEQ_SEQ_FORWARD(pg_compact_sequence_send, seq_send_impl)
BIOSEQ_SEQ_FORWARD(pg_compact_sequence_cast, seq_cast_impl)
BIOSEQ_SEQ_FORWARD(pg_compact_sequence_cmp, seq_cmp_impl)
BIOSEQ_SEQ_FORWARD(pg_compact_sequence_lt, seq_lt_impl)
BIOSEQ_SEQ_FORWARD(pg_compact_sequence_le, seq_le_impl)
BIOSEQ_SEQ_FORWARD(pg_compact_sequence_eq, seq_eq_impl)
BIOSEQ_SEQ_FORWARD(pg_compact_sequence_ne, seq_ne_impl)
BIOSEQ_SEQ_FORWARD(pg_compact_sequence_ge, seq_ge_impl)
BIOSEQ_SEQ_FORWARD(pg_compact_sequence_gt, seq_gt_impl)
BIOSEQ_SEQ_FORWARD(pg_compact_sequence_len, seq_len_impl)
BIOSEQ_SEQ_FORWARD(pg_compact_sequence_unit_len, seq_unit_len_impl)
BIOSEQ_SEQ_FORWARD(seq_hash, seq_hash_impl)
BIOSEQ_SEQ_FORWARD(seq_hash_extended, seq_hash_extended_impl)

BIOSEQ_SEQ_FORWARD(letter_typmod_in, letter_typmod_in_impl)
BIOSEQ_SEQ_FORWARD(letter_typmod_out, letter_typmod_out_impl)
BIOSEQ_SEQ_FORWARD(letter_in, letter_in_impl)
BIOSEQ_SEQ_FORWARD(letter_out, letter_out_impl)
BIOSEQ_SEQ_FORWARD(letter_recv, letter_recv_impl)
BIOSEQ_SEQ_FORWARD(letter_send, letter_send_impl)
BIOSEQ_SEQ_FORWARD(letter_typmod_cast, letter_typmod_cast_impl)
BIOSEQ_SEQ_FORWARD(letter_to_text, letter_to_text_impl)
BIOSEQ_SEQ_FORWARD(letter_cmp, letter_cmp_impl)
BIOSEQ_SEQ_FORWARD(letter_eq, letter_eq_impl)
BIOSEQ_SEQ_FORWARD(letter_ne, letter_ne_impl)
BIOSEQ_SEQ_FORWARD(letter_lt, letter_lt_impl)
BIOSEQ_SEQ_FORWARD(letter_le, letter_le_impl)
BIOSEQ_SEQ_FORWARD(letter_gt, letter_gt_impl)
BIOSEQ_SEQ_FORWARD(letter_ge, letter_ge_impl)
BIOSEQ_SEQ_FORWARD(letter_hash, letter_hash_impl)
BIOSEQ_SEQ_FORWARD(letter_hash_extended, letter_hash_extended_impl)

BIOSEQ_SEQ_FORWARD(seq_subscript_handler, seq_subscript_handler_impl)
BIOSEQ_SEQ_FORWARD(seq_concat, seq_concat_impl)
BIOSEQ_SEQ_FORWARD(seq_concat_text, seq_concat_text_impl)
BIOSEQ_SEQ_FORWARD(text_concat_seq, text_concat_seq_impl)
BIOSEQ_SEQ_FORWARD(seq_concat_varchar, seq_concat_varchar_impl)
BIOSEQ_SEQ_FORWARD(varchar_concat_seq, varchar_concat_seq_impl)
BIOSEQ_SEQ_FORWARD(seq_concat_text_array, seq_concat_text_array_impl)
BIOSEQ_SEQ_FORWARD(text_array_concat_seq, text_array_concat_seq_impl)
BIOSEQ_SEQ_FORWARD(seq_concat_letter_array, seq_concat_letter_array_impl)
BIOSEQ_SEQ_FORWARD(letter_array_concat_seq, letter_array_concat_seq_impl)
BIOSEQ_SEQ_FORWARD(seq_reverse, seq_reverse_impl)
BIOSEQ_SEQ_FORWARD(seq_reverse_complement, seq_reverse_complement_impl)
BIOSEQ_SEQ_FORWARD(seq_array_length, seq_array_length_impl)
BIOSEQ_SEQ_FORWARD(seq_cardinality, seq_cardinality_impl)
BIOSEQ_SEQ_FORWARD(seq_to_text, seq_to_text_impl)
BIOSEQ_SEQ_FORWARD(seq_to_varchar, seq_to_varchar_impl)
BIOSEQ_SEQ_FORWARD(seq_to_array, seq_to_array_impl)
BIOSEQ_SEQ_FORWARD(seq_from_letter_array_infer,
                   seq_from_letter_array_infer_impl)
BIOSEQ_SEQ_FORWARD(seq_from_letter_array_named,
                   seq_from_letter_array_named_impl)

#undef BIOSEQ_SEQ_FORWARD
