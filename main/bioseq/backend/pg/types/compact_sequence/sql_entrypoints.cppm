/* PostgreSQL fmgr metadata, C entrypoints, and exception translation. */
module;

#include <exception>

#include "headers/pg_compat.h"
#include "fmgr_boundary.h"

export module bioseq.types.pg_compact_sequence:sql_entrypoints;

import bioseq.pg_utils;

/* Keep this emitter independent of the heavy owner partitions.  The owner
 * modules publish only the declaration-only C ABI from fmgr_boundary.h; this
 * unit emits PostgreSQL metadata/wrappers without loading their BMIs. */
extern "C" {
[[nodiscard, gnu::used, gnu::noinline]]
PGDLLEXPORT Datum bioseq_invoke_pg_sequence_fmgr_boundary(
    Datum (*implementation)(FunctionCallInfo),
    FunctionCallInfo fcinfo)
{
    ErrorData* exact_error = nullptr;
    bool standard_error = false;
    bool unknown_error = false;
    char detail[512] = "std::exception without detail";
    try {
        return implementation(fcinfo);
    } catch (bioseq::pg_utils::PostgresErrorAsCpp& error) {
        exact_error = error.release_error_data();
    } catch (std::exception const& error) {
        strlcpy(detail,
                error.what() == nullptr
                    ? "std::exception without detail"
                    : error.what(),
                sizeof(detail));
        standard_error = true;
    } catch (...) {
        unknown_error = true;
    }
    if (exact_error != nullptr) {
        ReThrowError(exact_error);
    }
    ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("bioseq sequence callback raised a C++ exception"),
             errdetail("%s", unknown_error
                     ? "unknown non-standard C++ exception"
                     : (standard_error ? detail
                                       : "empty C++ exception state"))));
    pg_unreachable();
}
} // extern "C"

extern "C" {
CXX_PG_FUNCTION_INFO_V1(pg_compact_sequence_typmod_in);

PGDLLEXPORT Datum pg_compact_sequence_typmod_in(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(&seq_typmod_in_impl, fcinfo);
}
} // extern "C"

extern "C" {
CXX_PG_FUNCTION_INFO_V1(pg_compact_sequence_typmod_out);

PGDLLEXPORT Datum pg_compact_sequence_typmod_out(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(&seq_typmod_out_impl, fcinfo);
}
} // extern "C"

extern "C" {
CXX_PG_FUNCTION_INFO_V1(pg_compact_sequence_in);
PGDLLEXPORT Datum pg_compact_sequence_in(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(&seq_in_impl, fcinfo);
}

CXX_PG_FUNCTION_INFO_V1(pg_compact_sequence_out);
PGDLLEXPORT Datum pg_compact_sequence_out(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(&seq_out_impl, fcinfo);
}

CXX_PG_FUNCTION_INFO_V1(pg_compact_sequence_recv);
PGDLLEXPORT Datum pg_compact_sequence_recv(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(&seq_recv_impl, fcinfo);
}

CXX_PG_FUNCTION_INFO_V1(pg_compact_sequence_send);
PGDLLEXPORT Datum pg_compact_sequence_send(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(&seq_send_impl, fcinfo);
}

CXX_PG_FUNCTION_INFO_V1(pg_compact_sequence_cast);
PGDLLEXPORT Datum pg_compact_sequence_cast(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(&seq_cast_impl, fcinfo);
}

CXX_PG_FUNCTION_INFO_V1(pg_compact_sequence_len);
PGDLLEXPORT Datum pg_compact_sequence_len(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(&seq_len_impl, fcinfo);
}

CXX_PG_FUNCTION_INFO_V1(pg_compact_sequence_unit_len);
PGDLLEXPORT Datum pg_compact_sequence_unit_len(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(&seq_unit_len_impl, fcinfo);
}
} // extern "C"

extern "C" {
CXX_PG_FUNCTION_INFO_V1(pg_compact_sequence_cmp);
PGDLLEXPORT Datum pg_compact_sequence_cmp(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(&seq_cmp_impl, fcinfo);
}

CXX_PG_FUNCTION_INFO_V1(pg_compact_sequence_lt);
PGDLLEXPORT Datum pg_compact_sequence_lt(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(&seq_lt_impl, fcinfo);
}

CXX_PG_FUNCTION_INFO_V1(pg_compact_sequence_le);
PGDLLEXPORT Datum pg_compact_sequence_le(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(&seq_le_impl, fcinfo);
}

CXX_PG_FUNCTION_INFO_V1(pg_compact_sequence_eq);
PGDLLEXPORT Datum pg_compact_sequence_eq(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(&seq_eq_impl, fcinfo);
}

CXX_PG_FUNCTION_INFO_V1(pg_compact_sequence_ne);
PGDLLEXPORT Datum pg_compact_sequence_ne(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(&seq_ne_impl, fcinfo);
}

CXX_PG_FUNCTION_INFO_V1(pg_compact_sequence_ge);
PGDLLEXPORT Datum pg_compact_sequence_ge(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(&seq_ge_impl, fcinfo);
}

CXX_PG_FUNCTION_INFO_V1(pg_compact_sequence_gt);
PGDLLEXPORT Datum pg_compact_sequence_gt(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(&seq_gt_impl, fcinfo);
}

CXX_PG_FUNCTION_INFO_V1(seq_hash);
PGDLLEXPORT Datum seq_hash(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(&seq_hash_impl, fcinfo);
}

CXX_PG_FUNCTION_INFO_V1(seq_hash_extended);
PGDLLEXPORT Datum seq_hash_extended(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(&seq_hash_extended_impl, fcinfo);
}
} // extern "C"

extern "C" {
CXX_PG_FUNCTION_INFO_V1(letter_typmod_in);
PGDLLEXPORT Datum letter_typmod_in(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(&letter_typmod_in_impl, fcinfo);
}

CXX_PG_FUNCTION_INFO_V1(letter_typmod_out);
PGDLLEXPORT Datum letter_typmod_out(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(&letter_typmod_out_impl, fcinfo);
}

CXX_PG_FUNCTION_INFO_V1(letter_in);
PGDLLEXPORT Datum letter_in(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(&letter_in_impl, fcinfo);
}

CXX_PG_FUNCTION_INFO_V1(letter_out);
PGDLLEXPORT Datum letter_out(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(&letter_out_impl, fcinfo);
}

CXX_PG_FUNCTION_INFO_V1(letter_recv);
PGDLLEXPORT Datum letter_recv(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(&letter_recv_impl, fcinfo);
}

CXX_PG_FUNCTION_INFO_V1(letter_send);
PGDLLEXPORT Datum letter_send(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(&letter_send_impl, fcinfo);
}

CXX_PG_FUNCTION_INFO_V1(letter_wire_self_test);
PGDLLEXPORT Datum letter_wire_self_test(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(&letter_wire_self_test_impl, fcinfo);
}

CXX_PG_FUNCTION_INFO_V1(letter_hook_chain_calls);
PGDLLEXPORT Datum letter_hook_chain_calls(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(&letter_hook_chain_calls_impl, fcinfo);
}

CXX_PG_FUNCTION_INFO_V1(letter_typmod_cast);
PGDLLEXPORT Datum letter_typmod_cast(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(&letter_typmod_cast_impl, fcinfo);
}

CXX_PG_FUNCTION_INFO_V1(letter_to_text);
PGDLLEXPORT Datum letter_to_text(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(&letter_to_text_impl, fcinfo);
}

CXX_PG_FUNCTION_INFO_V1(letter_cmp);
PGDLLEXPORT Datum letter_cmp(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(&letter_cmp_impl, fcinfo);
}

CXX_PG_FUNCTION_INFO_V1(letter_eq);
PGDLLEXPORT Datum letter_eq(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(&letter_eq_impl, fcinfo);
}

CXX_PG_FUNCTION_INFO_V1(letter_ne);
PGDLLEXPORT Datum letter_ne(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(&letter_ne_impl, fcinfo);
}

CXX_PG_FUNCTION_INFO_V1(letter_lt);
PGDLLEXPORT Datum letter_lt(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(&letter_lt_impl, fcinfo);
}

CXX_PG_FUNCTION_INFO_V1(letter_le);
PGDLLEXPORT Datum letter_le(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(&letter_le_impl, fcinfo);
}

CXX_PG_FUNCTION_INFO_V1(letter_gt);
PGDLLEXPORT Datum letter_gt(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(&letter_gt_impl, fcinfo);
}

CXX_PG_FUNCTION_INFO_V1(letter_ge);
PGDLLEXPORT Datum letter_ge(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(&letter_ge_impl, fcinfo);
}

CXX_PG_FUNCTION_INFO_V1(letter_hash);
PGDLLEXPORT Datum letter_hash(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(&letter_hash_impl, fcinfo);
}

CXX_PG_FUNCTION_INFO_V1(letter_hash_extended);
PGDLLEXPORT Datum letter_hash_extended(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(&letter_hash_extended_impl, fcinfo);
}
} // extern "C"

extern "C" {
CXX_PG_FUNCTION_INFO_V1(seq_subscript_handler);
PGDLLEXPORT Datum seq_subscript_handler(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(&seq_subscript_handler_impl, fcinfo);
}
} // extern "C"

extern "C" {
CXX_PG_FUNCTION_INFO_V1(seq_concat);
PGDLLEXPORT Datum seq_concat(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(&seq_concat_impl, fcinfo);
}

CXX_PG_FUNCTION_INFO_V1(seq_concat_text);
PGDLLEXPORT Datum seq_concat_text(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(&seq_concat_text_impl, fcinfo);
}

CXX_PG_FUNCTION_INFO_V1(text_concat_seq);
PGDLLEXPORT Datum text_concat_seq(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(&text_concat_seq_impl, fcinfo);
}

CXX_PG_FUNCTION_INFO_V1(seq_concat_varchar);
PGDLLEXPORT Datum seq_concat_varchar(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(&seq_concat_varchar_impl, fcinfo);
}

CXX_PG_FUNCTION_INFO_V1(varchar_concat_seq);
PGDLLEXPORT Datum varchar_concat_seq(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(&varchar_concat_seq_impl, fcinfo);
}

CXX_PG_FUNCTION_INFO_V1(seq_reverse);
PGDLLEXPORT Datum seq_reverse(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(&seq_reverse_impl, fcinfo);
}

CXX_PG_FUNCTION_INFO_V1(seq_reverse_complement);
PGDLLEXPORT Datum seq_reverse_complement(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(&seq_reverse_complement_impl, fcinfo);
}
} // extern "C"

extern "C" {
CXX_PG_FUNCTION_INFO_V1(seq_concat_text_array);
PGDLLEXPORT Datum seq_concat_text_array(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(&seq_concat_text_array_impl, fcinfo);
}

CXX_PG_FUNCTION_INFO_V1(text_array_concat_seq);
PGDLLEXPORT Datum text_array_concat_seq(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(&text_array_concat_seq_impl, fcinfo);
}

CXX_PG_FUNCTION_INFO_V1(seq_concat_letter_array);
PGDLLEXPORT Datum seq_concat_letter_array(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(&seq_concat_letter_array_impl, fcinfo);
}

CXX_PG_FUNCTION_INFO_V1(letter_array_concat_seq);
PGDLLEXPORT Datum letter_array_concat_seq(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(&letter_array_concat_seq_impl, fcinfo);
}

CXX_PG_FUNCTION_INFO_V1(seq_array_length);
PGDLLEXPORT Datum seq_array_length(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(&seq_array_length_impl, fcinfo);
}

CXX_PG_FUNCTION_INFO_V1(seq_cardinality);
PGDLLEXPORT Datum seq_cardinality(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(&seq_cardinality_impl, fcinfo);
}

CXX_PG_FUNCTION_INFO_V1(seq_to_text);
PGDLLEXPORT Datum seq_to_text(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(&seq_to_text_impl, fcinfo);
}

CXX_PG_FUNCTION_INFO_V1(seq_to_varchar);
PGDLLEXPORT Datum seq_to_varchar(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(&seq_to_varchar_impl, fcinfo);
}

CXX_PG_FUNCTION_INFO_V1(seq_to_array);
PGDLLEXPORT Datum seq_to_array(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(&seq_to_array_impl, fcinfo);
}

CXX_PG_FUNCTION_INFO_V1(seq_from_letter_array_infer);
PGDLLEXPORT Datum seq_from_letter_array_infer(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(&seq_from_letter_array_infer_impl, fcinfo);
}

CXX_PG_FUNCTION_INFO_V1(seq_from_letter_array_named);
PGDLLEXPORT Datum seq_from_letter_array_named(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(&seq_from_letter_array_named_impl, fcinfo);
}
} // extern "C"

extern "C" {
CXX_PG_FUNCTION_INFO_V1(synthetic_lazy_sequence);
PGDLLEXPORT Datum synthetic_lazy_sequence(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(&synthetic_lazy_sequence_impl, fcinfo);
}

CXX_PG_FUNCTION_INFO_V1(sequence_piece_count);
PGDLLEXPORT Datum sequence_piece_count(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(&sequence_piece_count_impl, fcinfo);
}

CXX_PG_FUNCTION_INFO_V1(materialization_size_self_test);
PGDLLEXPORT Datum materialization_size_self_test(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(&materialization_size_self_test_impl, fcinfo);
}

CXX_PG_FUNCTION_INFO_V1(expanded_ownership_self_test);
PGDLLEXPORT Datum expanded_ownership_self_test(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(&expanded_ownership_self_test_impl, fcinfo);
}
} // extern "C"

extern "C" {
CXX_PG_FUNCTION_INFO_V1(piece_tree_self_test);
PGDLLEXPORT Datum piece_tree_self_test(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(&piece_tree_self_test_impl, fcinfo);
}
} // extern "C"

extern "C" {
CXX_PG_FUNCTION_INFO_V1(slice_trace_self_test);
PGDLLEXPORT Datum slice_trace_self_test(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(&slice_trace_self_test_impl, fcinfo);
}

CXX_PG_FUNCTION_INFO_V1(slice_payload_allocation_probe);
PGDLLEXPORT Datum slice_payload_allocation_probe(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(
        &slice_payload_allocation_probe_impl, fcinfo);
}

CXX_PG_FUNCTION_INFO_V1(slice_trace_worker_failure_probe);
PGDLLEXPORT Datum slice_trace_worker_failure_probe(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(
        &slice_trace_worker_failure_probe_impl, fcinfo);
}
} // extern "C"
