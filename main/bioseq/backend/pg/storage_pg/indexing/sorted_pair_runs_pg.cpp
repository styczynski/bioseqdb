/**
 * @file sorted_pair_runs_pg.cpp
 * @brief PCM-free PostgreSQL error leaf for the sorted-run module.
 */
#include "common.h"

extern "C" {

[[noreturn]] void bioseq_sorted_run_error_pg_leaf(
        unsigned error_code) {
    const char* message = "bioseq sorted runs: unknown error";
    switch (error_code) {
    case 0u:
        message = "bioseq sorted runs: position spill create failed";
        break;
    case 1u:
        message = "bioseq sorted runs: failed to rewind position spill";
        break;
    case 2u:
        message = "bioseq sorted runs: could not admit exact position buffers";
        break;
    case 3u:
        message = "bioseq sorted runs: BufFileCreateTemp returned null";
        break;
    case 4u:
        message = "bioseq sorted runs: admitted run without permit";
        break;
    case 5u:
        message = "bioseq sorted runs: failed to rewind BufFile";
        break;
    case 6u:
        message = "bioseq sorted runs: could not admit one merge reader";
        break;
    case 7u:
        message = "bioseq sorted runs: failed to rewind compacted BufFile";
        break;
    case 8u:
        message = "bioseq sorted runs: compact BufFile create failed";
        break;
    case 9u:
        message =
            "bioseq sorted runs: poisoned run store reused after PostgreSQL error";
        break;
    case 10u:
        message = "bioseq sorted runs: finalized run store cannot be reused";
        break;
    }

    ereport(ERROR,
        (errcode(ERRCODE_INTERNAL_ERROR),
         errmsg_internal("%s", message)));
    __builtin_unreachable();
}

} // extern "C"
