#pragma once

#include "headers/pg_compat.h"

extern "C" {
[[nodiscard]] Datum bioseq_invoke_pg_sequence_fmgr_boundary(
    Datum (*implementation)(FunctionCallInfo),
    FunctionCallInfo fcinfo);

/* Import-free C ABI between the owner partitions and :sql_entrypoints.
 * Owner modules retain the algorithm bodies; the SQL partition alone owns
 * fmgr metadata and the externally registered PostgreSQL wrappers. */
[[nodiscard]] PGDLLEXPORT Datum expanded_ownership_self_test_impl(FunctionCallInfo fcinfo);
[[nodiscard]] PGDLLEXPORT Datum letter_array_concat_seq_impl(FunctionCallInfo fcinfo);
[[nodiscard]] PGDLLEXPORT Datum letter_cmp_impl(FunctionCallInfo fcinfo);
[[nodiscard]] PGDLLEXPORT Datum letter_eq_impl(FunctionCallInfo fcinfo);
[[nodiscard]] PGDLLEXPORT Datum letter_ge_impl(FunctionCallInfo fcinfo);
[[nodiscard]] PGDLLEXPORT Datum letter_gt_impl(FunctionCallInfo fcinfo);
[[nodiscard]] PGDLLEXPORT Datum letter_hash_extended_impl(FunctionCallInfo fcinfo);
[[nodiscard]] PGDLLEXPORT Datum letter_hash_impl(FunctionCallInfo fcinfo);
[[nodiscard]] PGDLLEXPORT Datum letter_hook_chain_calls_impl(FunctionCallInfo fcinfo);
[[nodiscard]] PGDLLEXPORT Datum letter_in_impl(FunctionCallInfo fcinfo);
[[nodiscard]] PGDLLEXPORT Datum letter_le_impl(FunctionCallInfo fcinfo);
[[nodiscard]] PGDLLEXPORT Datum letter_lt_impl(FunctionCallInfo fcinfo);
[[nodiscard]] PGDLLEXPORT Datum letter_ne_impl(FunctionCallInfo fcinfo);
[[nodiscard]] PGDLLEXPORT Datum letter_out_impl(FunctionCallInfo fcinfo);
[[nodiscard]] PGDLLEXPORT Datum letter_recv_impl(FunctionCallInfo fcinfo);
[[nodiscard]] PGDLLEXPORT Datum letter_send_impl(FunctionCallInfo fcinfo);
[[nodiscard]] PGDLLEXPORT Datum letter_to_text_impl(FunctionCallInfo fcinfo);
[[nodiscard]] PGDLLEXPORT Datum letter_typmod_cast_impl(FunctionCallInfo fcinfo);
[[nodiscard]] PGDLLEXPORT Datum letter_typmod_in_impl(FunctionCallInfo fcinfo);
[[nodiscard]] PGDLLEXPORT Datum letter_typmod_out_impl(FunctionCallInfo fcinfo);
[[nodiscard]] PGDLLEXPORT Datum letter_wire_self_test_impl(FunctionCallInfo fcinfo);
[[nodiscard]] PGDLLEXPORT Datum materialization_size_self_test_impl(FunctionCallInfo fcinfo);
[[nodiscard]] PGDLLEXPORT Datum piece_tree_self_test_impl(FunctionCallInfo fcinfo);
[[nodiscard]] PGDLLEXPORT Datum seq_array_length_impl(FunctionCallInfo fcinfo);
[[nodiscard]] PGDLLEXPORT Datum seq_cardinality_impl(FunctionCallInfo fcinfo);
[[nodiscard]] PGDLLEXPORT Datum seq_cast_impl(FunctionCallInfo fcinfo);
[[nodiscard]] PGDLLEXPORT Datum seq_cmp_impl(FunctionCallInfo fcinfo);
[[nodiscard]] PGDLLEXPORT Datum seq_concat_impl(FunctionCallInfo fcinfo);
[[nodiscard]] PGDLLEXPORT Datum seq_concat_letter_array_impl(FunctionCallInfo fcinfo);
[[nodiscard]] PGDLLEXPORT Datum seq_concat_text_array_impl(FunctionCallInfo fcinfo);
[[nodiscard]] PGDLLEXPORT Datum seq_concat_text_impl(FunctionCallInfo fcinfo);
[[nodiscard]] PGDLLEXPORT Datum seq_concat_varchar_impl(FunctionCallInfo fcinfo);
[[nodiscard]] PGDLLEXPORT Datum seq_eq_impl(FunctionCallInfo fcinfo);
[[nodiscard]] PGDLLEXPORT Datum seq_from_letter_array_infer_impl(FunctionCallInfo fcinfo);
[[nodiscard]] PGDLLEXPORT Datum seq_from_letter_array_named_impl(FunctionCallInfo fcinfo);
[[nodiscard]] PGDLLEXPORT Datum seq_ge_impl(FunctionCallInfo fcinfo);
[[nodiscard]] PGDLLEXPORT Datum seq_gt_impl(FunctionCallInfo fcinfo);
[[nodiscard]] PGDLLEXPORT Datum seq_hash_extended_impl(FunctionCallInfo fcinfo);
[[nodiscard]] PGDLLEXPORT Datum seq_hash_impl(FunctionCallInfo fcinfo);
[[nodiscard]] PGDLLEXPORT Datum seq_in_impl(FunctionCallInfo fcinfo);
[[nodiscard]] PGDLLEXPORT Datum seq_le_impl(FunctionCallInfo fcinfo);
[[nodiscard]] PGDLLEXPORT Datum seq_len_impl(FunctionCallInfo fcinfo);
[[nodiscard]] PGDLLEXPORT Datum seq_lt_impl(FunctionCallInfo fcinfo);
[[nodiscard]] PGDLLEXPORT Datum seq_ne_impl(FunctionCallInfo fcinfo);
[[nodiscard]] PGDLLEXPORT Datum seq_out_impl(FunctionCallInfo fcinfo);
[[nodiscard]] PGDLLEXPORT Datum seq_recv_impl(FunctionCallInfo fcinfo);
[[nodiscard]] PGDLLEXPORT Datum seq_reverse_complement_impl(FunctionCallInfo fcinfo);
[[nodiscard]] PGDLLEXPORT Datum seq_reverse_impl(FunctionCallInfo fcinfo);
[[nodiscard]] PGDLLEXPORT Datum seq_send_impl(FunctionCallInfo fcinfo);
[[nodiscard]] PGDLLEXPORT Datum seq_subscript_handler_impl(FunctionCallInfo fcinfo);
[[nodiscard]] PGDLLEXPORT Datum seq_to_array_impl(FunctionCallInfo fcinfo);
[[nodiscard]] PGDLLEXPORT Datum seq_to_text_impl(FunctionCallInfo fcinfo);
[[nodiscard]] PGDLLEXPORT Datum seq_to_varchar_impl(FunctionCallInfo fcinfo);
[[nodiscard]] PGDLLEXPORT Datum seq_typmod_in_impl(FunctionCallInfo fcinfo);
[[nodiscard]] PGDLLEXPORT Datum seq_typmod_out_impl(FunctionCallInfo fcinfo);
[[nodiscard]] PGDLLEXPORT Datum seq_unit_len_impl(FunctionCallInfo fcinfo);
[[nodiscard]] PGDLLEXPORT Datum sequence_piece_count_impl(FunctionCallInfo fcinfo);
[[nodiscard]] PGDLLEXPORT Datum slice_payload_allocation_probe_impl(FunctionCallInfo fcinfo);
[[nodiscard]] PGDLLEXPORT Datum slice_trace_self_test_impl(FunctionCallInfo fcinfo);
[[nodiscard]] PGDLLEXPORT Datum slice_trace_worker_failure_probe_impl(FunctionCallInfo fcinfo);
[[nodiscard]] PGDLLEXPORT Datum synthetic_lazy_sequence_impl(FunctionCallInfo fcinfo);
[[nodiscard]] PGDLLEXPORT Datum text_array_concat_seq_impl(FunctionCallInfo fcinfo);
[[nodiscard]] PGDLLEXPORT Datum text_concat_seq_impl(FunctionCallInfo fcinfo);
[[nodiscard]] PGDLLEXPORT Datum varchar_concat_seq_impl(FunctionCallInfo fcinfo);
}
