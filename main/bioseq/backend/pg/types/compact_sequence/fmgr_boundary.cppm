/* C++ exception translation for compact-sequence PostgreSQL entrypoints. */
module;

#include <exception>

#include "headers/pg_compat.h"
#include "fmgr_boundary.h"

export module bioseq.types.pg_compact_sequence:fmgr_boundary;

import bioseq.pg_utils;

/* Catch C++ before entering PostgreSQL's terminal longjmp path. Only POD
 * translation state remains live when ReThrowError/ereport is invoked.
 * PostgresErrorAsCpp transfers its exact ErrorData, preserving SQLSTATE,
 * detail, hint, and context. */
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
