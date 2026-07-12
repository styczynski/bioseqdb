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
#include <span>
#include <string>
#include <vector>

export module bioseq.scheduler.dispatch_buffer_api;

import bioseq.sequences.simd;
import bioseq.sequences.minimap_port;
import bioseq.sequences.pipe;
import bioseq.indexing.type;

export namespace bioseq::scheduler {

struct DispatchNextOutput {
    TupleTableSlot* scan_slot_copy = nullptr;
    std::int64_t sequence_rowno = -1;
    std::uint64_t query_ordinal = 0;
    const bioseq::simd::active_target::MatchResult* match = nullptr;
};

struct DispatchJoinQueryRef {
    std::uint64_t ordinal = 0;
    TupleTableSlot* query_slot = nullptr;
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
        bioseq::indexing::IndexReadRequirement index_read_requirement,
        bool read_sequence_index,
        Oid scan_user_relid,
        Oid scan_user_database_oid,
        Oid scan_user_tablespace_oid,
        Oid scan_user_main_relfilenode,
        Snapshot executor_snapshot);

    void replace_executor_snapshot(Snapshot executor_snapshot);

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

    void configure_mmsearch_join(
        Oid target_relation_oid,
        AttrNumber target_storage_seq_attno,
        AttrNumber target_slot_seq_resno,
        AttrNumber target_slot_rowno_resno,
        AttrNumber query_slot_seq_resno,
        std::string target_column_name,
        std::uint8_t alphabet_code,
        std::uint8_t storage_unit_byte_width);
    void begin_join_target(TupleTableSlot* locator_passthrough_target);
    void prepare_join_target_queries(
        std::span<const DispatchJoinQueryRef> all_queries);
    void set_join_query_batch(std::span<const DispatchJoinQueryRef> queries);
    [[nodiscard]] bool run_join_query_batch_and_build_cursors();
    void finish_join_target();
    void reset_join_runtime_for_rescan();
    void abandon_join_runtime_after_pg_error_noexcept() noexcept;

    void submit_scan(TupleTableSlot* scan_slot);
    [[nodiscard]] bool ready_to_dispatch() const;
    [[nodiscard]] bool run_batch_and_build_cursors();
    void clear_slots();

    [[nodiscard]] std::optional<DispatchNextOutput> read_next_output();
    [[nodiscard]] bioseq::pipeline_stats::PipelineExecutionStats
    execution_stats() const;
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
