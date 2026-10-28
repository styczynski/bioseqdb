/**
 * @file typed_support_parts/pg_alignment_errors.cppm
 * @brief Reports alignment errors to PostgreSQL and prepares GPU CIGAR data.
 */
module;

extern "C" {
#include "postgres.h"

#include "utils/errcodes.h"
}

#undef Min
#undef Max
#undef snprintf
#undef vsnprintf

#include <new>

export module bioseq.postgres.scheduler.dispatch_processor.typed_support.pg_alignment_errors;

export import bioseq.postgres.scheduler.dispatch_processor.checked_alignment_calls;
import std;
import bioseq.postgres.scheduler.dispatch_processor.operational_stats;
import bioseq.common.seqan_adapter;
import bioseq.postgres.support;
import bioseq.postgres.storage.indexing.active_read_policy;
import bioseq.sequences;
import bioseq.sequences.cigar;
import bioseq.pipeline.minimap_port;
import bioseq.pipeline.pipe;
import bioseq.gpu.vulkan.runtime;
import bioseq.storage.lazy_sequence;
import bioseq.storage.size;

export namespace bioseq::simd::typed_dispatch::typed_support_detail {

/** Reports that the required stored minimizer index is unavailable for this row. */
template <class p_scanner_pg_context>
[[noreturn]] inline void
raise_required_persisted_index_error(const p_scanner_pg_context* scanner_pg_ctx,
                                     const std::int64_t target_row,
                                     std::string_view reason) {
    const std::string_view active_index =
        scanner_pg_ctx != nullptr && !scanner_pg_ctx->active_index.empty()
            ? std::string_view{scanner_pg_ctx->active_index}
            : std::string_view{"<unresolved>"};
    const std::string_view policy_source =
        scanner_pg_ctx != nullptr
            ? bioseq::storage::indexing::active_read_policy_resolution_source_name(
                  scanner_pg_ctx->active_index_policy_source)
            : std::string_view{"no_scanner_context"};
    const std::string detail =
        std::format("index_read_requirement=require_compatible_persisted "
                    "target_row={} active_index={} active_index_policy_source={} reason={}",
                    static_cast<long long>(target_row),
                    active_index,
                    policy_source,
                    reason);
    raise_typed_dispatch_pg_error_as_cpp(
        ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
        "required compatible persisted minimizer index unavailable",
        detail.c_str(),
        "Publish and wire a validated current READY generation reader, "
        "or choose index_read_requirement=allow/require_absent; pair-sort "
        "rebuild and recompute fallbacks are forbidden.");
}

/** Counts which index source was used and whether a requested stored index was missing. */
inline void
record_target_index_outcome(::bioseq::pipeline_stats::pipeline_execution_stats& stats,
                            const ::bioseq::pipeline_stats::target_index_source requested_source,
                            const ::bioseq::pipeline_stats::target_index_source actual_source,
                            const std::uint64_t rows = 1) noexcept {
    if (actual_source == ::bioseq::pipeline_stats::target_index_source::persisted) {
        add_operational_counter(stats, stats.operational.persisted_index_rows, rows);
    } else if (actual_source == ::bioseq::pipeline_stats::target_index_source::recomputed) {
        add_operational_counter(stats, stats.operational.recomputed_target_index_rows, rows);
    }
    if (requested_source == ::bioseq::pipeline_stats::target_index_source::persisted &&
        actual_source != ::bioseq::pipeline_stats::target_index_source::persisted) {
        add_operational_counter(stats, stats.operational.missing_persisted_index_rows, rows);
    }
}

/** Runs ordinary minimap alignment and reports known failures to PostgreSQL. */
template <class... p_arguments>
[[nodiscard]] match_result_collection minimap_port_raw_pg_checked(p_arguments&&... args) {
    return run_aligner_pg_checked("minimap_port", [&args...]() -> match_result_collection {
        return ::bioseq::simd::minimap_port_raw_owned(std::forward<p_arguments>(args)...);
    });
}

/** Runs alignment with a hash-major index and reports known failures to PostgreSQL. */
template <class... p_arguments>
[[nodiscard]] match_result_collection minimap_port_hash_major_pg_checked(p_arguments&&... args) {
    return run_aligner_pg_checked(
        "minimap2_port_with_index", [&args...]() -> match_result_collection {
            return ::bioseq::simd::minimap_port_hash_major_owned(
                std::forward<p_arguments>(args)...);
        });
}

/** Runs alignment with a grouped-SoA index and reports known failures to PostgreSQL. */
template <class... p_arguments>
[[nodiscard]] match_result_collection minimap_port_grouped_soa_pg_checked(p_arguments&&... args) {
    return run_aligner_pg_checked(
        "minimap2_port_with_index", [&args...]() -> match_result_collection {
            return ::bioseq::simd::minimap_port_grouped_soa_owned(
                std::forward<p_arguments>(args)...);
        });
}

/**
 * Runs @p function and closes its fragment storage afterward A PostgreSQL
 * error abandons the reader safely. Other exceptions trigger a close attempt
 * before the original exception  is rethrown.
 */
template <class p_storage, class p_function>
inline void with_checked_minimizer_fragment_reader(p_storage& storage, p_function&& function) {
    try {
        std::forward<p_function>(function)();
        storage.close_checked();
    } catch (const bioseq::postgres::support::postgres_error&) {
        storage.abandon_after_pg_error_noexcept();
        throw;
    } catch (...) {
        try {
            storage.close_checked();
        } catch (...) {
            storage.abandon_after_pg_error_noexcept();
        }
        throw;
    }
}

} // namespace bioseq::simd::typed_dispatch::typed_support_detail
