/**
 * @file
 * @brief Give custom scans access to the batch buffer without importing its implementation.
 */
module;

#include <bioseq/pg_compat.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

export module bioseq.postgres.scheduler.dispatch_buffer.runtime_contract;

export import bioseq.postgres.scheduler.dispatch_buffer.output_contract;

import bioseq.pipeline.minimap_parameters_contract;
import bioseq.pipeline.pipe;
import bioseq.pipeline.stats;
import bioseq.indexing.type;
import bioseq.postgres.types.compact_sequence;
import bioseq.postgres.storage.scanner.source_provider_contract;
import bioseq.pipeline.runtime_contract;

export namespace bioseq::scheduler {

/** @return The registered Highway CPU dispatch plan. */
[[nodiscard]] bioseq::simd_dispatch::highway_dispatch_plan
highway_dispatch_plan_for_plan() noexcept;

/**
 * @brief Attach Highway kernels and create the statement's fixed execution plan.
 * @param[in] parameters Validated executor settings with per-call options applied.
 * @return The shared statement plan used by later alignment calls.
 */
[[nodiscard]] bioseq::simd::pipe::statement_execution_plan_owner
make_postgres_statement_execution_plan(bioseq::simd::minimap2_parameters parameters);

/** @brief Associates a query ordinal with a borrowed PostgreSQL tuple slot. */
struct dispatch_join_query_ref {
    std::uint64_t ordinal = 0;
    TupleTableSlot* query_slot = nullptr;
};

/**
 * @brief Own the batch buffer used by custom scans.
 *
 * The runtime can be moved. Its borrowed relation, snapshot, and memory
 * context must remain valid while it executes. Copying is disabled.
 */
class dispatch_buffer_runtime {
  public:
    /** @brief Create an empty buffer and reserve space from the estimated row count. */
    explicit dispatch_buffer_runtime(std::size_t estimated_tuple_count);
    ~dispatch_buffer_runtime();

    dispatch_buffer_runtime(const dispatch_buffer_runtime&) = delete;
    dispatch_buffer_runtime& operator=(const dispatch_buffer_runtime&) = delete;
    dispatch_buffer_runtime(dispatch_buffer_runtime&& other) noexcept;
    dispatch_buffer_runtime& operator=(dispatch_buffer_runtime&& other) noexcept;

    /** @brief Save the alignment plan and borrow the executor state needed for storage reads. */
    void configure_params(bioseq::simd::pipe::statement_execution_plan_owner execution_plan,
                          bioseq::indexing::index_read_requirement index_read_requirement,
                          bool read_sequence_index,
                          Oid scan_user_relid,
                          Oid scan_user_database_oid,
                          Oid scan_user_tablespace_oid,
                          Oid scan_user_main_relfilenode,
                          Relation executor_relation,
                          Snapshot executor_snapshot,
                          MemoryContext executor_query_context);

    /** @brief Replaces the borrowed executor snapshot used for later reads. */
    void replace_executor_snapshot(Snapshot executor_snapshot);

    /**
     * @brief Borrow the statement's live sequence provider.
     * The executor keeps ownership and closes it after its readers finish.
     */
    void bind_sequence_source_provider(bioseq::storage::pg_sequence_source_provider* provider);
    /** @brief Clears the borrowed sequence source provider without closing it. */
    void clear_sequence_source_provider_binding() noexcept;

    /** @brief Share the statement's counters for GPU operations on stored indexes. */
    void bind_persisted_gpu_operation_stats_sink(
        bioseq::pipeline_stats::persisted_gpu_operation_accumulator& sink);

    /** @brief Share the statement timer for downstream processing. */
    void bind_downstream_active_wall_tracker(
        bioseq::simd::downstream_active_wall_tracker& tracker);

    /** @brief Attach the owner that tracks asynchronous slice jobs and payload allocations. */
    void bind_slice_async_owner(slice_async_owner_guard* owner);
    /** @brief Release the per-row slice owner after the batch is drained. */
    void release_per_row_slice_async_owner_after_drain();
    /** @brief Release the join slice owner after all target work is drained. */
    void release_join_slice_async_owner_after_drain();

    /** @brief Configure mmsearch to reuse one constant query across target rows. */
    void configure_mmsearch(AttrNumber seq_attno,
                            std::string column_name,
                            std::vector<char> query_varlena_bytes,
                            std::uint8_t alphabet_code,
                            std::uint8_t storage_unit_byte_width,
                            AttrNumber primary_key_attno);

    /** @brief Configure mmsearch to take its target and query from each input row. */
    void configure_mmsearch_per_row(AttrNumber seq_attno,
                                    std::string column_name,
                                    AttrNumber per_row_query_attno,
                                    bool per_row_self_alignment,
                                    std::uint8_t alphabet_code,
                                    std::uint8_t storage_unit_byte_width,
                                    std::uint8_t query_alphabet_code,
                                    std::uint8_t query_storage_unit_byte_width,
                                    AttrNumber primary_key_attno);

    /**
     * @brief Configure mmsearch to accept evaluated sequence expressions per row.
     * seq_attno still identifies the source and output column. Each submission
     * supplies the expression values used for alignment.
     */
    void configure_mmsearch_per_row_evaluated(AttrNumber seq_attno,
                                              std::string column_name,
                                              std::uint8_t alphabet_code,
                                              std::uint8_t storage_unit_byte_width,
                                              std::uint8_t query_alphabet_code,
                                              std::uint8_t query_storage_unit_byte_width,
                                              AttrNumber primary_key_attno);

    /** @brief Install query bytes already converted to the target storage width. */
    void apply_typmod_coalesce(std::vector<char> coalesced_bytes);
    /** @brief Clears mmsearch state and optionally removes extra queries. */
    void clear_mmsearch_context(bool clear_extras = false);
    /** @brief Add a constant query that each target row must also match. */
    void add_extra_const_query(std::vector<char> query_varlena_bytes);

    /** @brief Configures the target and query slot layout for a batch join. */
    void configure_mmsearch_join(Oid target_relation_oid,
                                 AttrNumber target_storage_seq_attno,
                                 AttrNumber target_slot_seq_resno,
                                 AttrNumber target_slot_rowno_resno,
                                 AttrNumber query_slot_seq_resno,
                                 std::string target_column_name,
                                 std::uint8_t alphabet_code,
                                 std::uint8_t storage_unit_byte_width,
                                 std::uint8_t query_alphabet_code,
                                 std::uint8_t query_storage_unit_byte_width);
    /** @brief Copy one locator-backed target row for reuse across query batches. */
    void begin_join_target(TupleTableSlot* locator_passthrough_target);
    /** @brief Prepare the shared target for the queries in this join pass. */
    void prepare_join_target_queries(std::span<const dispatch_join_query_ref> all_queries);
    /** @brief Selects the next query batch for the current join target. */
    void set_join_query_batch(std::span<const dispatch_join_query_ref> queries);
    /**
     * @brief Align the selected query batch and prepare its output cursors.
     * @return True when a nonempty batch ran, even if it found no matches.
     */
    [[nodiscard]] bool run_join_query_batch_and_build_cursors();
    /** @brief Check and release the current target's preparation and buffered rows. */
    void finish_join_target();
    /** @brief Clear join inputs and results for a rescan, keeping the statement configuration. */
    void reset_join_runtime_for_rescan();
    /** @brief Discard join state after PostgreSQL ERROR and abort active sequence reads. */
    void abandon_join_runtime_after_pg_error_noexcept() noexcept;

    /** @brief Copies one physical scan slot into the pending dispatch batch. */
    void submit_scan(TupleTableSlot* scan_slot);
    /**
     * @brief Copy a transformed row with its source relation and tuple ID.
     * Use the tuple ID for index reads only when @p source_relation_oid matches
     * this buffer's relation. Other source rows keep no direct row identity.
     */
    void submit_scan_with_source_tid(TupleTableSlot* scan_slot,
                                     Oid source_relation_oid,
                                     ItemPointerData source_tid);
    /**
     * @brief Copy a transformed row without assigning a physical source identity.
     * Ignore any tuple ID in the slot because its relation is unknown.
     */
    void submit_scan_recipe_without_source_tid(TupleTableSlot* scan_slot);
    /** @brief Copies one scan slot together with evaluated target and query values. */
    void submit_scan_evaluated(TupleTableSlot* scan_slot,
                               Datum target,
                               bool target_is_null,
                               bool has_query_override,
                               Datum query,
                               bool query_is_null);
    /** @return Whether the current scan batch has reached its row limit. */
    [[nodiscard]] bool ready_to_dispatch() const;
    /**
     * @brief Align the buffered scan rows and prepare output cursors.
     * @return True when mmsearch ran on a nonempty buffer, regardless of matches.
     */
    [[nodiscard]] bool run_batch_and_build_cursors();
    /** @brief Release the batch's copied rows and results, keeping mmsearch configuration. */
    void clear_slots();

    /**
     * @brief Return the next copied row and match.
     * @return Borrowed pointers, or nullopt after the last output.
     */
    [[nodiscard]] std::optional<dispatch_next_output> read_next_output();
    /** @return A copy of the last batch's execution counters, including all its waves. */
    [[nodiscard]] bioseq::pipeline_stats::pipeline_execution_stats execution_stats() const;
    /** @return Whether the configured policy permits reading stored sequence indexes. */
    [[nodiscard]] bool reads_sequence_index() const;
    /** @return The number of constant filters in addition to the primary query. */
    [[nodiscard]] std::size_t extra_query_count() const;
    /** @return Whether the current batch has finished collecting input and can be drained. */
    [[nodiscard]] bool has_finished_writing() const;
    /** @return Whether mmsearch is configured for this buffer. */
    [[nodiscard]] bool mmsearch_active() const;
    /** @return Whether mmsearch uses the same source column for both operands. */
    [[nodiscard]] bool per_row_self_alignment() const;
    /** @return Whether a converted constant query is active. */
    [[nodiscard]] bool coalesce_active() const;
    /** @return The target alphabet code. */
    [[nodiscard]] std::uint8_t mmsearch_alphabet_code() const;
    /** @return The target storage-unit width in bytes. */
    [[nodiscard]] std::uint8_t mmsearch_storage_unit_byte_width() const;
    /** @return The target attribute number, or InvalidAttrNumber when unavailable. */
    [[nodiscard]] AttrNumber target_sequence_attno() const noexcept;
    /** @return The per-row query attribute number, or InvalidAttrNumber when unavailable. */
    [[nodiscard]] AttrNumber per_row_query_sequence_attno() const noexcept;

  private:
    /** Owned tuple_dispatch_buffer, accessed through the implementation module. */
    void* implementation = nullptr;
};

} // namespace bioseq::scheduler
