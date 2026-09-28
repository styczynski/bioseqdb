/**
 * @file
 * @brief Provides base-scan runtime access and cleanup when query memory is reset.
 */
module;
// Include Highway here so its target-specific definitions share the, same
// translation-unit context across imported modules.
extern "C" {
#include "postgres.h"
#include "nodes/execnodes.h"
}

#undef Min
#undef Max
#undef snprintf
#undef vsnprintf

#include <hwy/highway.h>

export module bioseq.postgres.table:custom_scan_runtime_support;

export import :custom_scan_plan_abi_constants;
export import :custom_scan_runtime_buffer_contracts;
export import :custom_scan_runtime_placeholder_contracts;
export import :custom_scan_runtime_match_contracts;
export import :custom_scan_scan_feature_contracts;
export import :custom_scan_scan_runtime_state_contracts;
export import :custom_scan_runtime_pipeline_contracts;
export import :custom_scan_execution_stats_contracts;
export import :custom_scan_batch_join_execution_runtime_state_contracts;
export import :custom_scan_per_row_join_runtime_contracts;
export import :custom_scan_aligner_options_codec_contracts;

import bioseq.common.logging;
import bioseq.postgres.storage.scanner;
import bioseq.postgres.table.scan_state;
import bioseq.postgres.types.compact_sequence;

export {

    /* Keep C++ runtime cleanup tied to PostgreSQL's querc lifetime. */

    /**
     * @brief Returns the scan's C++ runtime, allocating it on first access.
     *
     * The pointer is stored separately from PostgreSQL's scan-state layout. Call
     * register_runtime_cleanup_callback to give the allocation query-context
     * cleanup.
     */
    [[nodiscard]] bioseq_scan_runtime& bioseq_runtime(scan_state* state) {
        if (state->runtime_state == nullptr) {
            bioseq_log<logging_level::debug>(
                "[bioseq] pg: bioseq_runtime() lazy-allocating runtime — BeginCustomScan did not "
                "fire first; this allocation will leak unless a cleanup callback is wired");
            state->runtime_state = static_cast<void*>(new bioseq_scan_runtime{});
        }
        return *static_cast<bioseq_scan_runtime*>(state->runtime_state);
    }

    /**
     * @brief Destroys the scan's C++ runtime when query memory is reset.
     *
     * Normal executor shutdown  leaves this allocation alive because the reset
     * callback is stored inside it Close any remaining sequence provider and
     * release its trace attachment before deleting the runtime.
     */
    void bioseq_runtime_cleanup_callback(void* arg) {
        auto* state = static_cast<scan_state*>(arg);
        if (state == nullptr || state->runtime_state == nullptr) {
            return;
        }
        auto* runtime_ptr = static_cast<bioseq_scan_runtime*>(state->runtime_state);
        if (runtime_ptr->cleanup_done) {
            bioseq_log<logging_level::debug>("[bioseq] pg: bioseq_runtime_cleanup_callback() "
                                             "normal-path delete after EndCustomScan");
        } else {
            bioseq_log<logging_level::debug>("[bioseq] pg: bioseq_runtime_cleanup_callback() "
                                             "abort-path delete (slots not released)");
        }
        if (runtime_ptr->lazy_source_provider != nullptr) {
            bioseq::storage::close_pg_sequence_source_provider(
                runtime_ptr->lazy_source_provider,
                /*is_commit=*/runtime_ptr->cleanup_done);
            runtime_ptr->lazy_source_provider = nullptr;
        }
        if (runtime_ptr->lazy_trace_attachment != nullptr) {
            release_slice_trace_attachment(runtime_ptr->lazy_trace_attachment);
            runtime_ptr->lazy_trace_attachment = nullptr;
        }
        state->runtime_state = nullptr;
        delete runtime_ptr;
    }

    /**
     * @brief Registers query-context cleanup for the scan's runtime.
     *
     * Allocate the runtime if needed and register its embedded callback once for
     * this scan state. The callback will run when estate's query memory resets.
     */
    void register_runtime_cleanup_callback(scan_state* state, EState* estate) {
        if (state == nullptr || estate == nullptr) {
            return;
        }
        bioseq_scan_runtime& runtime = bioseq_runtime(state);
        if (runtime.owning_state == state) {
            return; /* already registered */
        }
        runtime.owning_state = state;
        runtime.memcontext_callback.func = bioseq_runtime_cleanup_callback;
        runtime.memcontext_callback.arg = static_cast<void*>(state);
        MemoryContextRegisterResetCallback(estate->es_query_cxt, &runtime.memcontext_callback);
    }

} // export custom-scan partition declarations
