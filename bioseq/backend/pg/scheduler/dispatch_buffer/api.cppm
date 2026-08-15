/**
 * @file
 * @brief Declares the custom-scan API for buffered sequence dispatch.
 */
/*-------------------------------------------------------------------------
 *
 * scheduler/dispatch_buffer/api.cppm
 *
 * Narrow custom-scan-facing API for tuple_dispatch_buffer.  The primary
 * goal is to keep table/custom_scan.cppm from importing the full
 * bioseq.postgres.scheduler.dispatch_buffer BMI: GCC 16.1 crashes while loading
 * that heavy exported module from custom_scan, but the executor only
 * needs an opaque buffer handle plus a small row/match view.
 *
 *-------------------------------------------------------------------------
 */
module;

#include <bioseq/pg_compat.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

export module bioseq.postgres.scheduler.dispatch_buffer.api;

import bioseq.pipeline.minimap_port;
import bioseq.pipeline.pipe;
import bioseq.indexing.type;
import bioseq.postgres.types.compact_sequence;
import bioseq.postgres.storage.scanner;
import bioseq.highway.dispatch_contract;

export namespace bioseq::scheduler {

/**
 * @brief Returns the registered Highway plan without exposing the processor module.
 * @return The immutable registered dispatch plan.
 */
[[nodiscard]] bioseq::simd_dispatch::highway_dispatch_plan
highway_dispatch_plan_for_plan() noexcept;

/**
 * @brief Borrows one buffered scan row and its current alignment match.
 *
 * The buffer owns both pointers and keeps them valid until its next state
 * transition.
 */
struct dispatch_next_output {
    TupleTableSlot* scan_slot_copy = nullptr;
    std::int64_t sequence_rowno = -1;
    std::uint64_t query_ordinal = 0;
    const bioseq::simd::match_result* match = nullptr;
};

/** @brief Associates a query ordinal with a borrowed PostgreSQL tuple slot. */
struct dispatch_join_query_ref {
    std::uint64_t ordinal = 0;
    TupleTableSlot* query_slot = nullptr;
};

/**
 * @brief Owns the opaque tuple dispatch buffer used by custom scans.
 *
 * The runtime is movable, not copyable, and must remain inside the PostgreSQL
 * executor lifetime that supplied its borrowed relation and memory context.
 */
class dispatch_buffer_runtime {
public:
    /** @brief Creates an empty runtime sized for the estimated tuple count. */
    explicit dispatch_buffer_runtime(std::size_t estimated_tuple_count);
    ~dispatch_buffer_runtime();

    dispatch_buffer_runtime(const dispatch_buffer_runtime&) = delete;
    dispatch_buffer_runtime& operator=(const dispatch_buffer_runtime&) = delete;
    dispatch_buffer_runtime(dispatch_buffer_runtime&& rhs) noexcept;
    dispatch_buffer_runtime& operator=(dispatch_buffer_runtime&& rhs) noexcept;

    /** @brief Configures alignment, index-read, relation, snapshot, and memory owners. */
    void configure_params(
        const bioseq::simd::minimap2_parameters& params,
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
     * @brief Binds the statement-owned sequence source provider.
     *
     * The runtime borrows the provider and never closes it independently.
     */
    void bind_sequence_source_provider(
        bioseq::storage::pg_sequence_source_provider* provider);
    /** @brief Clears the borrowed sequence source provider without closing it. */
    void clear_sequence_source_provider_binding() noexcept;

    /** @brief Binds the statement accumulator for persisted GPU operation statistics. */
    void bind_persisted_gpu_operation_stats_sink(
        bioseq::pipeline_stats::persisted_gpu_operation_accumulator& sink);

    /** @brief Binds the statement guard that owns asynchronous slice work. */
    void bind_slice_async_owner(slice_async_owner_guard* owner);
    /** @brief Releases drained per-row asynchronous slice ownership. */
    void release_per_row_slice_async_owner_after_drain();
    /** @brief Releases drained join asynchronous slice ownership. */
    void release_join_slice_async_owner_after_drain();

    /** @brief Configures a broadcast-query mmsearch scan. */
    void configure_mmsearch(
        AttrNumber seq_attno,
        std::string column_name,
        std::vector<char> query_varlena_bytes,
        std::uint8_t alphabet_code,
        std::uint8_t storage_unit_byte_width,
        AttrNumber primary_key_attno);

    /** @brief Configures a per-row mmsearch scan backed by physical attributes. */
    void configure_mmsearch_per_row(
        AttrNumber seq_attno,
        std::string column_name,
        AttrNumber per_row_query_attno,
        bool per_row_self_alignment,
        std::uint8_t alphabet_code,
        std::uint8_t storage_unit_byte_width,
        std::uint8_t query_alphabet_code,
        std::uint8_t query_storage_unit_byte_width,
        AttrNumber primary_key_attno);

    /**
     * @brief Configures a per-row mmsearch whose operands are evaluated expressions.
     *
     * The sequence attribute still identifies source authority and preserves
     * raw output. It does not select either evaluated operand.
     */
    void configure_mmsearch_per_row_evaluated(
        AttrNumber seq_attno,
        std::string column_name,
        std::uint8_t alphabet_code,
        std::uint8_t storage_unit_byte_width,
        std::uint8_t query_alphabet_code,
        std::uint8_t query_storage_unit_byte_width,
        AttrNumber primary_key_attno);

    /** @brief Installs one coalesced query value for the active mmsearch. */
    void apply_typmod_coalesce(std::vector<char> coalesced_bytes);
    /** @brief Clears mmsearch state and optionally removes extra queries. */
    void clear_mmsearch_context(bool clear_extras = false);
    /** @brief Adds another constant query to the current broadcast batch. */
    void add_extra_const_query(std::vector<char> query_varlena_bytes);

    /** @brief Configures the target and query slot layout for a batch join. */
    void configure_mmsearch_join(
        Oid target_relation_oid,
        AttrNumber target_storage_seq_attno,
        AttrNumber target_slot_seq_resno,
        AttrNumber target_slot_rowno_resno,
        AttrNumber query_slot_seq_resno,
        std::string target_column_name,
        std::uint8_t alphabet_code,
        std::uint8_t storage_unit_byte_width,
        std::uint8_t query_alphabet_code,
        std::uint8_t query_storage_unit_byte_width);
    /** @brief Starts buffering query batches for one borrowed join target slot. */
    void begin_join_target(TupleTableSlot* locator_passthrough_target);
    /** @brief Prepares all query references needed by the current join target. */
    void prepare_join_target_queries(
        std::span<const dispatch_join_query_ref> all_queries);
    /** @brief Selects the next query batch for the current join target. */
    void set_join_query_batch(std::span<const dispatch_join_query_ref> queries);
    /**
     * @brief Executes the selected join batch and constructs output cursors.
     * @return True when at least one output cursor is available.
     */
    [[nodiscard]] bool run_join_query_batch_and_build_cursors();
    /** @brief Finishes the current join target and releases its transient state. */
    void finish_join_target();
    /** @brief Resets join state while retaining reusable statement ownership. */
    void reset_join_runtime_for_rescan();
    /** @brief Abandons join state after PostgreSQL ERROR without invoking PostgreSQL APIs. */
    void abandon_join_runtime_after_pg_error_noexcept() noexcept;

    /** @brief Copies one physical scan slot into the pending dispatch batch. */
    void submit_scan(TupleTableSlot* scan_slot);
    /** @brief Copies one scan slot together with evaluated target and query values. */
    void submit_scan_evaluated(
        TupleTableSlot* scan_slot,
        Datum target,
        bool target_is_null,
        bool has_query_override,
        Datum query,
        bool query_is_null);
    /**
     * @brief Reports whether the current scan batch reached dispatch capacity.
     * @return True when the batch should be executed.
     */
    [[nodiscard]] bool ready_to_dispatch() const;
    /**
     * @brief Executes the current scan batch and constructs output cursors.
     * @return True when at least one output cursor is available.
     */
    [[nodiscard]] bool run_batch_and_build_cursors();
    /** @brief Releases all copied scan slots and row-local state. */
    void clear_slots();

    /**
     * @brief Advances to the next buffered output row.
     * @return A borrowed row view or absence after the cursors are drained.
     */
    [[nodiscard]] std::optional<dispatch_next_output> read_next_output();
    /**
     * @brief Returns accumulated pipeline statistics for the last dispatch.
     * @return A value snapshot independent of later buffer mutations.
     */
    [[nodiscard]] bioseq::pipeline_stats::pipeline_execution_stats
    execution_stats() const;
    /**
     * @brief Counts additional broadcast queries.
     * @return The number of queries beyond the primary query.
     */
    [[nodiscard]] std::size_t extra_query_count() const;
    /**
     * @brief Reports whether the buffer completed its writing stage.
     * @return True after no more input slots may be submitted.
     */
    [[nodiscard]] bool has_finished_writing() const;
    /**
     * @brief Reports whether mmsearch is configured for this buffer.
     * @return True when an mmsearch context is active.
     */
    [[nodiscard]] bool mmsearch_active() const;
    /**
     * @brief Reports whether each row supplies its own query sequence.
     * @return True for per-row self-alignment mode.
     */
    [[nodiscard]] bool per_row_self_alignment() const;
    /**
     * @brief Reports whether typmod coalescing supplied the active query.
     * @return True when a coalesced query value is installed.
     */
    [[nodiscard]] bool coalesce_active() const;
    /**
     * @brief Reads the active target alphabet code.
     * @return The encoded target alphabet identifier.
     */
    [[nodiscard]] std::uint8_t mmsearch_alphabet_code() const;
    /**
     * @brief Reads the active target storage-unit width.
     * @return The storage-unit width in bytes.
     */
    [[nodiscard]] std::uint8_t mmsearch_storage_unit_byte_width() const;
    /**
     * @brief Reads the physical target sequence attribute number.
     * @return The target attribute number or an invalid attribute when absent.
     */
    [[nodiscard]] AttrNumber target_sequence_attno() const noexcept;
    /**
     * @brief Reads the physical per-row query attribute number.
     * @return The query attribute number or an invalid attribute when absent.
     */
    [[nodiscard]] AttrNumber per_row_query_sequence_attno() const noexcept;

private:
    /** Opaque ownership pointer defined in the implementation module unit. */
    void* impl = nullptr;
};

} // namespace bioseq::scheduler
