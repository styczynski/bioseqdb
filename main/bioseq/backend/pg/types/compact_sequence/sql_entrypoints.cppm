/* Thin C-linkage fmgr entrypoints.  V1 records are registered centrally by
 * pg/mod.cppm so this partition contains no registration side effects. */
module;

#include "headers/pg_compat.h"

export module bioseq.types.pg_compact_sequence:sql_entrypoints;

import :trace;
import :tree_ops;
import :packed_output;
import :view_session;
import :io;
import :content;
import :letter;
import :subscript;
import :concat_reverse;
import :array_string;

namespace {

template <Datum (*Implementation)(FunctionCallInfo)>
[[nodiscard]] inline Datum
invoke_compact_sequence_entrypoint(FunctionCallInfo fcinfo) {
  return invoke_pg_sequence_fmgr_boundary(
      [fcinfo]() -> Datum { return Implementation(fcinfo); });
}

} // namespace

extern "C" PGDLLEXPORT Datum pg_compact_sequence_typmod_in(PG_FUNCTION_ARGS) {
  return invoke_compact_sequence_entrypoint<&seq_typmod_in_impl>(fcinfo);
}
extern "C" PGDLLEXPORT Datum pg_compact_sequence_typmod_out(PG_FUNCTION_ARGS) {
  return invoke_compact_sequence_entrypoint<&seq_typmod_out_impl>(fcinfo);
}
extern "C" PGDLLEXPORT Datum pg_compact_sequence_in(PG_FUNCTION_ARGS) {
  return invoke_compact_sequence_entrypoint<&seq_in_impl>(fcinfo);
}
extern "C" PGDLLEXPORT Datum pg_compact_sequence_out(PG_FUNCTION_ARGS) {
  return invoke_compact_sequence_entrypoint<&seq_out_impl>(fcinfo);
}
extern "C" PGDLLEXPORT Datum pg_compact_sequence_recv(PG_FUNCTION_ARGS) {
  return invoke_compact_sequence_entrypoint<&seq_recv_impl>(fcinfo);
}
extern "C" PGDLLEXPORT Datum pg_compact_sequence_send(PG_FUNCTION_ARGS) {
  return invoke_compact_sequence_entrypoint<&seq_send_impl>(fcinfo);
}
extern "C" PGDLLEXPORT Datum pg_compact_sequence_cast(PG_FUNCTION_ARGS) {
  return invoke_compact_sequence_entrypoint<&seq_cast_impl>(fcinfo);
}
extern "C" PGDLLEXPORT Datum pg_compact_sequence_cmp(PG_FUNCTION_ARGS) {
  return invoke_compact_sequence_entrypoint<&seq_cmp_impl>(fcinfo);
}
extern "C" PGDLLEXPORT Datum pg_compact_sequence_lt(PG_FUNCTION_ARGS) {
  return invoke_compact_sequence_entrypoint<&seq_lt_impl>(fcinfo);
}
extern "C" PGDLLEXPORT Datum pg_compact_sequence_le(PG_FUNCTION_ARGS) {
  return invoke_compact_sequence_entrypoint<&seq_le_impl>(fcinfo);
}
extern "C" PGDLLEXPORT Datum pg_compact_sequence_eq(PG_FUNCTION_ARGS) {
  return invoke_compact_sequence_entrypoint<&seq_eq_impl>(fcinfo);
}
extern "C" PGDLLEXPORT Datum pg_compact_sequence_ne(PG_FUNCTION_ARGS) {
  return invoke_compact_sequence_entrypoint<&seq_ne_impl>(fcinfo);
}
extern "C" PGDLLEXPORT Datum pg_compact_sequence_ge(PG_FUNCTION_ARGS) {
  return invoke_compact_sequence_entrypoint<&seq_ge_impl>(fcinfo);
}
extern "C" PGDLLEXPORT Datum pg_compact_sequence_gt(PG_FUNCTION_ARGS) {
  return invoke_compact_sequence_entrypoint<&seq_gt_impl>(fcinfo);
}
extern "C" PGDLLEXPORT Datum pg_compact_sequence_len(PG_FUNCTION_ARGS) {
  return invoke_compact_sequence_entrypoint<&seq_len_impl>(fcinfo);
}
extern "C" PGDLLEXPORT Datum pg_compact_sequence_unit_len(PG_FUNCTION_ARGS) {
  return invoke_compact_sequence_entrypoint<&seq_unit_len_impl>(fcinfo);
}
extern "C" PGDLLEXPORT Datum seq_hash(PG_FUNCTION_ARGS) {
  return invoke_compact_sequence_entrypoint<&seq_hash_impl>(fcinfo);
}
extern "C" PGDLLEXPORT Datum seq_hash_extended(PG_FUNCTION_ARGS) {
  return invoke_compact_sequence_entrypoint<&seq_hash_extended_impl>(fcinfo);
}
extern "C" PGDLLEXPORT Datum slice_trace_self_test(PG_FUNCTION_ARGS) {
  return invoke_compact_sequence_entrypoint<&slice_trace_self_test_impl>(
      fcinfo);
}
extern "C" PGDLLEXPORT Datum synthetic_lazy_sequence(PG_FUNCTION_ARGS) {
  return invoke_compact_sequence_entrypoint<&synthetic_lazy_sequence_impl>(
      fcinfo);
}
extern "C" PGDLLEXPORT Datum sequence_piece_count(PG_FUNCTION_ARGS) {
  return invoke_compact_sequence_entrypoint<&sequence_piece_count_impl>(fcinfo);
}
extern "C" PGDLLEXPORT Datum piece_tree_self_test(PG_FUNCTION_ARGS) {
  return invoke_compact_sequence_entrypoint<&piece_tree_self_test_impl>(fcinfo);
}
extern "C" PGDLLEXPORT Datum expanded_ownership_self_test(PG_FUNCTION_ARGS) {
  return invoke_compact_sequence_entrypoint<&expanded_ownership_self_test_impl>(
      fcinfo);
}

extern "C" PGDLLEXPORT Datum letter_typmod_in(PG_FUNCTION_ARGS) {
  return invoke_compact_sequence_entrypoint<&letter_typmod_in_impl>(fcinfo);
}
extern "C" PGDLLEXPORT Datum letter_typmod_out(PG_FUNCTION_ARGS) {
  return invoke_compact_sequence_entrypoint<&letter_typmod_out_impl>(fcinfo);
}
extern "C" PGDLLEXPORT Datum letter_in(PG_FUNCTION_ARGS) {
  return invoke_compact_sequence_entrypoint<&letter_in_impl>(fcinfo);
}
extern "C" PGDLLEXPORT Datum letter_out(PG_FUNCTION_ARGS) {
  return invoke_compact_sequence_entrypoint<&letter_out_impl>(fcinfo);
}
extern "C" PGDLLEXPORT Datum letter_recv(PG_FUNCTION_ARGS) {
  return invoke_compact_sequence_entrypoint<&letter_recv_impl>(fcinfo);
}
extern "C" PGDLLEXPORT Datum letter_send(PG_FUNCTION_ARGS) {
  return invoke_compact_sequence_entrypoint<&letter_send_impl>(fcinfo);
}
extern "C" PGDLLEXPORT Datum letter_wire_self_test(PG_FUNCTION_ARGS) {
  return invoke_compact_sequence_entrypoint<&letter_wire_self_test_impl>(
      fcinfo);
}
extern "C" PGDLLEXPORT Datum letter_typmod_cast(PG_FUNCTION_ARGS) {
  return invoke_compact_sequence_entrypoint<&letter_typmod_cast_impl>(fcinfo);
}
extern "C" PGDLLEXPORT Datum letter_to_text(PG_FUNCTION_ARGS) {
  return invoke_compact_sequence_entrypoint<&letter_to_text_impl>(fcinfo);
}
extern "C" PGDLLEXPORT Datum letter_cmp(PG_FUNCTION_ARGS) {
  return invoke_compact_sequence_entrypoint<&letter_cmp_impl>(fcinfo);
}
extern "C" PGDLLEXPORT Datum letter_eq(PG_FUNCTION_ARGS) {
  return invoke_compact_sequence_entrypoint<&letter_eq_impl>(fcinfo);
}
extern "C" PGDLLEXPORT Datum letter_ne(PG_FUNCTION_ARGS) {
  return invoke_compact_sequence_entrypoint<&letter_ne_impl>(fcinfo);
}
extern "C" PGDLLEXPORT Datum letter_lt(PG_FUNCTION_ARGS) {
  return invoke_compact_sequence_entrypoint<&letter_lt_impl>(fcinfo);
}
extern "C" PGDLLEXPORT Datum letter_le(PG_FUNCTION_ARGS) {
  return invoke_compact_sequence_entrypoint<&letter_le_impl>(fcinfo);
}
extern "C" PGDLLEXPORT Datum letter_gt(PG_FUNCTION_ARGS) {
  return invoke_compact_sequence_entrypoint<&letter_gt_impl>(fcinfo);
}
extern "C" PGDLLEXPORT Datum letter_ge(PG_FUNCTION_ARGS) {
  return invoke_compact_sequence_entrypoint<&letter_ge_impl>(fcinfo);
}
extern "C" PGDLLEXPORT Datum letter_hash(PG_FUNCTION_ARGS) {
  return invoke_compact_sequence_entrypoint<&letter_hash_impl>(fcinfo);
}
extern "C" PGDLLEXPORT Datum letter_hash_extended(PG_FUNCTION_ARGS) {
  return invoke_compact_sequence_entrypoint<&letter_hash_extended_impl>(fcinfo);
}

extern "C" PGDLLEXPORT Datum seq_subscript_handler(PG_FUNCTION_ARGS) {
  return invoke_compact_sequence_entrypoint<&seq_subscript_handler_impl>(
      fcinfo);
}
extern "C" PGDLLEXPORT Datum seq_concat(PG_FUNCTION_ARGS) {
  return invoke_compact_sequence_entrypoint<&seq_concat_impl>(fcinfo);
}
extern "C" PGDLLEXPORT Datum seq_concat_text(PG_FUNCTION_ARGS) {
  return invoke_compact_sequence_entrypoint<&seq_concat_text_impl>(fcinfo);
}
extern "C" PGDLLEXPORT Datum text_concat_seq(PG_FUNCTION_ARGS) {
  return invoke_compact_sequence_entrypoint<&text_concat_seq_impl>(fcinfo);
}
extern "C" PGDLLEXPORT Datum seq_concat_varchar(PG_FUNCTION_ARGS) {
  return invoke_compact_sequence_entrypoint<&seq_concat_varchar_impl>(fcinfo);
}
extern "C" PGDLLEXPORT Datum varchar_concat_seq(PG_FUNCTION_ARGS) {
  return invoke_compact_sequence_entrypoint<&varchar_concat_seq_impl>(fcinfo);
}
extern "C" PGDLLEXPORT Datum seq_concat_text_array(PG_FUNCTION_ARGS) {
  return invoke_compact_sequence_entrypoint<&seq_concat_text_array_impl>(
      fcinfo);
}
extern "C" PGDLLEXPORT Datum text_array_concat_seq(PG_FUNCTION_ARGS) {
  return invoke_compact_sequence_entrypoint<&text_array_concat_seq_impl>(
      fcinfo);
}
extern "C" PGDLLEXPORT Datum seq_concat_letter_array(PG_FUNCTION_ARGS) {
  return invoke_compact_sequence_entrypoint<&seq_concat_letter_array_impl>(
      fcinfo);
}
extern "C" PGDLLEXPORT Datum letter_array_concat_seq(PG_FUNCTION_ARGS) {
  return invoke_compact_sequence_entrypoint<&letter_array_concat_seq_impl>(
      fcinfo);
}
extern "C" PGDLLEXPORT Datum seq_reverse(PG_FUNCTION_ARGS) {
  return invoke_compact_sequence_entrypoint<&seq_reverse_impl>(fcinfo);
}
extern "C" PGDLLEXPORT Datum seq_reverse_complement(PG_FUNCTION_ARGS) {
  return invoke_compact_sequence_entrypoint<&seq_reverse_complement_impl>(
      fcinfo);
}
extern "C" PGDLLEXPORT Datum seq_array_length(PG_FUNCTION_ARGS) {
  return invoke_compact_sequence_entrypoint<&seq_array_length_impl>(fcinfo);
}
extern "C" PGDLLEXPORT Datum seq_cardinality(PG_FUNCTION_ARGS) {
  return invoke_compact_sequence_entrypoint<&seq_cardinality_impl>(fcinfo);
}
extern "C" PGDLLEXPORT Datum seq_to_text(PG_FUNCTION_ARGS) {
  return invoke_compact_sequence_entrypoint<&seq_to_text_impl>(fcinfo);
}
extern "C" PGDLLEXPORT Datum seq_to_varchar(PG_FUNCTION_ARGS) {
  return invoke_compact_sequence_entrypoint<&seq_to_varchar_impl>(fcinfo);
}
extern "C" PGDLLEXPORT Datum seq_to_array(PG_FUNCTION_ARGS) {
  return invoke_compact_sequence_entrypoint<&seq_to_array_impl>(fcinfo);
}
extern "C" PGDLLEXPORT Datum seq_from_letter_array_infer(PG_FUNCTION_ARGS) {
  return invoke_compact_sequence_entrypoint<&seq_from_letter_array_infer_impl>(
      fcinfo);
}
extern "C" PGDLLEXPORT Datum seq_from_letter_array_named(PG_FUNCTION_ARGS) {
  return invoke_compact_sequence_entrypoint<&seq_from_letter_array_named_impl>(
      fcinfo);
}
