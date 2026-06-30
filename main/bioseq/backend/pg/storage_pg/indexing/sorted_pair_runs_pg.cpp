/**
 * @file sorted_pair_runs_pg.cpp
 * @brief PostgreSQL logging implementation for the sorted-run module.
 */
module bioseq.storage.indexing_pg.sorted_runs;

import bioseq.common.mlogging;

namespace bioseq::storage::indexing {

void report_sorted_run_error(SortedRunError error) {
    switch (error) {
    case SortedRunError::position_spill_create_failed:
        bioseq_log<ERR, PG>("bioseq sorted runs: position spill create failed");
        return;
    case SortedRunError::position_spill_rewind_failed:
        bioseq_log<ERR, PG>("bioseq sorted runs: failed to rewind position spill");
        return;
    case SortedRunError::temp_file_create_failed:
        bioseq_log<ERR, PG>("bioseq sorted runs: BufFileCreateTemp returned null");
        return;
    case SortedRunError::admitted_run_without_permit:
        bioseq_log<ERR, PG>("bioseq sorted runs: admitted run without permit");
        return;
    case SortedRunError::run_rewind_failed:
        bioseq_log<ERR, PG>("bioseq sorted runs: failed to rewind BufFile");
        return;
    case SortedRunError::merge_reader_admission_failed:
        bioseq_log<ERR, PG>("bioseq sorted runs: could not admit one merge reader");
        return;
    case SortedRunError::compacted_run_rewind_failed:
        bioseq_log<ERR, PG>("bioseq sorted runs: failed to rewind compacted BufFile");
        return;
    case SortedRunError::compact_temp_file_create_failed:
        bioseq_log<ERR, PG>("bioseq sorted runs: compact BufFile create failed");
        return;
    }

    bioseq_log<ERR, PG>("bioseq sorted runs: unknown error");
}

} // namespace bioseq::storage::indexing
