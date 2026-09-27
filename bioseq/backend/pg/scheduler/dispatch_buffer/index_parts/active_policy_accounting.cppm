/**
 * @file index_parts/active_policy_accounting.cppm
 * @brief Record the selected index policy in the scanner and dispatch counters.
 */
module;

#include <bioseq/pg_compat.h>

#include <cstdint>
#include <utility>

export module bioseq.postgres.scheduler.dispatch_buffer:active_policy_accounting;

import bioseq.common.logging;
import bioseq.postgres.scheduler.dispatch_processor;
import bioseq.postgres.storage.indexing.active_read_policy_resolution_contract;

export namespace bioseq::scheduler {

inline void apply_active_read_policy_resolution(
    ::bioseq::simd::batch_processor::scanner_pg_context& context,
    bioseq::storage::indexing::active_read_policy_resolution resolution,
    const Oid user_relid,
    const Oid relfilenode,
    const std::int16_t attno) {
    std::uint64_t* counter = nullptr;
    switch (resolution.source) {
    case bioseq::storage::indexing::active_read_policy_source::configured_row:
        counter =
            &bioseq::storage::indexing::g_active_read_policy_dispatch_stats.configured_batches;
        break;
    case bioseq::storage::indexing::active_read_policy_source::authoritative_no_row_default:
        counter = &bioseq::storage::indexing::g_active_read_policy_dispatch_stats
                       .authoritative_default_batches;
        break;
    case bioseq::storage::indexing::active_read_policy_source::non_relation:
        counter =
            &bioseq::storage::indexing::g_active_read_policy_dispatch_stats.non_relation_batches;
        break;
    case bioseq::storage::indexing::active_read_policy_source::unresolved:
        break;
    }
    if (counter == nullptr) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("active-read policy resolver returned no dispatch source")));
        pg_unreachable();
    }
    ++*counter;
    context.active_index = std::move(resolution.index_name);
    context.active_index_policy_source = resolution.source;
    bioseq::storage::indexing::g_last_active_read_policy_dispatch_source = resolution.source;
    bioseq_log<logging_level::debug>(
        "[bioseq] dispatch active-read policy resolution source={} relation={} relfilenode={} "
        "attno={} index={} source_dispatch_count={}",
        bioseq::storage::indexing::active_read_policy_resolution_source_name(resolution.source),
        static_cast<unsigned>(user_relid),
        static_cast<unsigned>(relfilenode),
        static_cast<int>(attno),
        context.active_index,
        static_cast<unsigned long long>(*counter));
}

} // namespace bioseq::scheduler
