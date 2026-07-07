/*-------------------------------------------------------------------------
 *
 * scheduler/dispatch_buffer_api.cppm
 *
 * Narrow custom-scan-facing API for TupleDispatchBuffer.  The primary
 * goal is to keep table/custom_scan.cppm from importing the full
 * bioseq.scheduler.dispatch_buffer BMI: GCC 16.1 crashes while loading
 * that heavy exported module from custom_scan, but the executor only
 * needs an opaque buffer handle plus a small row/match view.
 *
 *-------------------------------------------------------------------------
 */
module;

#include "common.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

export module bioseq.scheduler.dispatch_buffer_api;

import bioseq.sequences.simd;
import bioseq.sequences.minimap_port;

export namespace bioseq::scheduler {

struct DispatchGpuBatchExecutionStats {
    bool gpu_batched_branch_taken = false;
    std::uint64_t gpu_problem_count = 0;
    std::uint64_t gpu_wave_count = 0;
    std::uint64_t gpu_segment_indexes_built = 0;
    std::uint64_t gpu_segment_indexes_reused = 0;
    std::uint64_t gpu_seed_dispatches = 0;
    std::uint64_t gpu_chain_dispatches = 0;
    std::uint64_t gpu_extend_dispatches = 0;
    std::uint64_t cpu_fallback_rows = 0;
    std::uint64_t simd_fallback_rows = 0;
    std::uint64_t vulkan_declines = 0;
    std::uint64_t gpu_batch_pipeline_ns = 0;
    std::uint64_t gpu_query_sketch_ns = 0;
    std::uint64_t gpu_target_dedup_ns = 0;
    std::uint64_t gpu_target_plan_ns = 0;
    std::uint64_t gpu_index_build_ns = 0;
    std::uint64_t gpu_problem_build_ns = 0;
    std::uint64_t gpu_seed_chain_runtime_ns = 0;
    std::uint64_t gpu_cigar_jobs = 0;
    std::uint64_t gpu_cigar_bounded_jobs = 0;
    std::uint64_t gpu_cigar_exact_jobs = 0;
    std::uint64_t gpu_cigar_unit_gap_jobs = 0;
    std::uint64_t gpu_cigar_gridded_jobs = 0;
    std::uint64_t gpu_cigar_query_symbols = 0;
    std::uint64_t gpu_cigar_target_symbols = 0;
    std::uint64_t gpu_cigar_packed_words = 0;
    std::uint64_t gpu_cigar_traceback_words = 0;
    std::uint64_t gpu_cigar_prepare_ns = 0;
    std::uint64_t gpu_cigar_symbol_pack_ns = 0;
    std::uint64_t gpu_cigar_classify_ns = 0;
    std::uint64_t gpu_cigar_runtime_ns = 0;
    std::uint64_t gpu_cigar_decode_ns = 0;
};

struct DispatchBatchExecutionStats {
    std::size_t targets_considered = 0;
    std::size_t targets_with_prebuilt_index = 0;
    std::size_t targets_recomputed_minimizers = 0;
    DispatchGpuBatchExecutionStats gpu{};
};

struct DispatchNextOutput {
    TupleTableSlot* scan_slot_copy = nullptr;
    std::int64_t sequence_rowno = -1;
    const bioseq::simd::active_target::MatchResult* match = nullptr;
};

class DispatchBufferRuntime {
public:
    explicit DispatchBufferRuntime(std::size_t estimated_tuple_count);
    ~DispatchBufferRuntime();

    DispatchBufferRuntime(const DispatchBufferRuntime&) = delete;
    DispatchBufferRuntime& operator=(const DispatchBufferRuntime&) = delete;
    DispatchBufferRuntime(DispatchBufferRuntime&& rhs) noexcept;
    DispatchBufferRuntime& operator=(DispatchBufferRuntime&& rhs) noexcept;

    void configure_params(
        const bioseq::simd::active_target::Minimap2Params& params,
        bool read_sequence_index,
        Oid scan_user_relid,
        Oid scan_user_database_oid,
        Oid scan_user_tablespace_oid,
        Oid scan_user_main_relfilenode);

    void configure_mmsearch(
        AttrNumber seq_attno,
        std::string column_name,
        std::vector<char> query_varlena_bytes,
        std::uint8_t alphabet_code,
        std::uint8_t storage_unit_byte_width,
        AttrNumber primary_key_attno);

    void configure_mmsearch_per_row(
        AttrNumber seq_attno,
        std::string column_name,
        AttrNumber per_row_query_attno,
        bool per_row_self_alignment,
        std::uint8_t alphabet_code,
        std::uint8_t storage_unit_byte_width,
        AttrNumber primary_key_attno);

    void apply_typmod_coalesce(std::vector<char> coalesced_bytes);
    void mark_alphabet_mismatch_recompute();
    void clear_mmsearch_context(bool clear_extras = false);
    void add_extra_const_query(std::vector<char> query_varlena_bytes);
    void submit_scan(TupleTableSlot* scan_slot);
    void run_batch_and_build_cursors();
    void clear_slots();

    [[nodiscard]] std::optional<DispatchNextOutput> read_next_output();
    [[nodiscard]] DispatchBatchExecutionStats execution_stats() const;
    [[nodiscard]] std::size_t extra_query_count() const;
    [[nodiscard]] bool has_finished_writing() const;
    [[nodiscard]] bool mmsearch_active() const;
    [[nodiscard]] bool per_row_self_alignment() const;
    [[nodiscard]] bool coalesce_active() const;
    [[nodiscard]] bool recompute_due_to_alphabet_mismatch() const;
    [[nodiscard]] std::uint8_t mmsearch_alphabet_code() const;
    [[nodiscard]] std::uint8_t mmsearch_storage_unit_byte_width() const;
    [[nodiscard]] std::uint8_t raw_query_alphabet_code() const;
    [[nodiscard]] std::uint8_t raw_query_storage_unit_byte_width() const;

private:
    void* impl = nullptr;
};

} // namespace bioseq::scheduler
