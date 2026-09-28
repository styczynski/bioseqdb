/**
 * @file
 * @brief Buffer scan rows, align them in batches, and return their matches.
 */
/*
 * submit_scan copies each accepted row so it survives reuse of the  scan slot.
 * At capacity or end of input, run_batch_and_build_cursors aligns the batch
 * and records matches by buffer entry. Rows without matches produce no output.
 *
 * read_next_output returns the copied row and its next match. The custom scan
 * fills the match-field placeholders before projecting the row, then clears
 * the batch after all results have been read.
 */
module;

#include <bioseq/pg_compat.h>
extern "C" {
#include "executor/instrument.h"
#include "utils/snapmgr.h"
}

#include <algorithm>
#include <array>
#include <cstring>
#include <exception>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// Locator-backed sequences are read through the statement source provider.
// bioseq.testing.chunked_scanner.enabled tests the same access pattern with
// synthetic fragments. bioseq.testing.in_memory_scanner.enabled assembles
// complete sequences in memory for  comprison.
export module bioseq.postgres.scheduler.dispatch_buffer;

export import :accounting;
export import :active_policy_accounting;

import bioseq.common.logging;
import bioseq.postgres.support.datums;
import bioseq.indexing.type;
import bioseq.pipeline.pipe;
import bioseq.postgres.storage.indexing.active_read_policy_resolution_contract;
import bioseq.postgres.storage.indexing.executor_read_context;
import bioseq.postgres.storage.indexing.minimizer_read_stats;
import bioseq.postgres.scheduler.dispatch_processor; // Keep the alignment pipeline in a separate module.
import bioseq.postgres.storage.manager;
import bioseq.storage.layout;
import bioseq.postgres.storage.scanner.source_provider_contract;
import bioseq.parallelism.memory_admission;
import bioseq.postgres.column.table_access;

export namespace bioseq::scheduler {

// Hold one batch of copied scan rows. The scan drains and clears each full
// batch before reading more rows from the same input
class tuple_dispatch_buffer {
  public:
    /*
     * mmsearch operands captured when the custom scan starts. Attribute numbers
     * identify the target and optional primary-key columns. A constant query
     * is copied here so later batches can reuse it. Target and query type
     * fields describe their alphabets and storage widths.
     */
    struct mmsearch_context {
        AttrNumber seq_attno = InvalidAttrNumber;
        AttrNumber primary_key_attno = InvalidAttrNumber;
        std::string column_name;
        std::vector<char>
            query_varlena; // Owned bytes of the detoasted query.
        uint8_t alphabet_code = 0;
        uint8_t storage_unit_byte_width = 0;
        uint8_t query_alphabet_code = 0;
        uint8_t query_storage_unit_byte_width = 0;
        bool active = false;

        /*
         * For mmsearch(seq_a, seq_b), read the query from seq_b in each row.
         * When both arguments name the  same column, per_row_self_alignment
         * records that, fact for EXPLAIN. The aligner still runs so SQL match
         * fields contain its actual result.
         */
        AttrNumber per_row_query_attno = InvalidAttrNumber;
        bool per_row_self_alignment = false;
        bool per_row_evaluated = false;

        /*
         * A query with the same alphabet may need a different storage width
         * to match the target column. Keep those converted bytes here while
         * preserving the original query. query_hdr() selects the converted
         * value when coalesce_active is set.
         */
        std::vector<char> coalesced_query_varlena;
        bool coalesce_active = false;
        [[nodiscard]] const bioseq::pg_types::seq* query_hdr() const {
            const std::vector<char>& bytes =
                coalesce_active ? coalesced_query_varlena : query_varlena;
            if (!active || bytes.empty()) {
                return nullptr;
            }
            return reinterpret_cast<const bioseq::pg_types::seq*>(bytes.data());
        }
    };

    /*
     * One accepted row and the sequences needed to align it. The copied slot
     * supplies other columns during otput projection. Sequence bytes or saved
     * slice wrappers survive  reuse of the original slot and resets of its
     * per-tuple memory context.
     */
    struct buffered_scan_entry {
        struct retained_expanded_attribute {
            AttrNumber attno = InvalidAttrNumber;
            Datum datum = Datum{0};
            pg_sequence_descriptor descriptor{};
        };
        TupleTableSlot* scan_slot_copy = nullptr;
        int32_t primary_key = -1;
        bool primary_key_is_null = true;
        int64_t sequence_rowno =
            -1; /* Direct-source row number, or -1 when the row has no matching physical source. */
        std::vector<char> seq_varlena;
        bool seq_is_null = true;
        bool target_from_evaluated_expression = false;

        /*
         * This row's query for mmsearch(seq_a, seq_b). Flat query bytes use
         * the target storage width for alignment The copied output slot
         * keeps the original query column value.
         */
        std::vector<char> per_row_query_varlena;
        bool per_row_query_is_null = true;
        bool query_from_evaluated_expression = false;

        /*
         * Locators let the source provider read sequence fragments as needed.
         * Keep them until dispatch. The scanner tests can fetch complete
         * payloads into the hydrated pointers. Small inline values stay
         * in seq_varlena.
         */
        std::optional<bioseq::storage::sequence_locator> seq_loc;
        std::optional<bioseq::storage::sequence_locator> per_row_query_loc;
        /*
         * Keep  a read-only wrapper for an expanded slice without flattening
         * its piece tree. The batch owns the wrapper and keeps its source
         * references until clear_slots() runs after output is drained.
         */
        Datum seq_recipe_datum = Datum{0};
        Datum per_row_query_recipe_datum = Datum{0};
        pg_sequence_descriptor seq_recipe_descriptor{};
        pg_sequence_descriptor per_row_query_recipe_descriptor{};
        std::vector<retained_expanded_attribute> expanded_sequence_attributes{};
        const bioseq::pg_types::seq* hydrated_seq_hdr = nullptr;
        const bioseq::pg_types::seq* hydrated_per_row_query_hdr = nullptr;

        [[nodiscard]] bool has_seq_recipe() const noexcept {
            return seq_recipe_datum != Datum{0};
        }

        [[nodiscard]] bool has_per_row_query_recipe() const noexcept {
            return per_row_query_recipe_datum != Datum{0};
        }

        [[nodiscard]] std::uint64_t seq_bp_size() const noexcept {
            if (const bioseq::pg_types::seq* header = seq_hdr(); header != nullptr) {
                return header->bp_size;
            }
            if (has_seq_recipe()) {
                return seq_recipe_descriptor.bp_length;
            }
            if (seq_loc.has_value()) {
                return seq_loc->logical_length;
            }
            return 0u;
        }

        [[nodiscard]] const bioseq::pg_types::seq* seq_hdr() const {
            if (hydrated_seq_hdr != nullptr) {
                return hydrated_seq_hdr;
            }
            if (seq_is_null || seq_varlena.empty()) {
                return nullptr;
            }
            return reinterpret_cast<const bioseq::pg_types::seq*>(seq_varlena.data());
        }

        [[nodiscard]] const bioseq::pg_types::seq* per_row_query_hdr() const {
            if (hydrated_per_row_query_hdr != nullptr) {
                return hydrated_per_row_query_hdr;
            }
            if (per_row_query_is_null || per_row_query_varlena.empty()) {
                return nullptr;
            }
            return reinterpret_cast<const bioseq::pg_types::seq*>(per_row_query_varlena.data());
        }
    };

    /* Select a match for one buffered row, in the order rows were submitted. */
    struct output_cursor {
        size_t scan_entry_index = 0;
        size_t match_index = 0;
    };

    struct next_output {
        buffered_scan_entry* entry;
        std::uint64_t query_ordinal;
        const ::bioseq::simd::match_result* match;
    };

    struct join_query_ref {
        std::uint64_t ordinal = 0;
        TupleTableSlot* query_slot = nullptr;
    };

    /* PG_TRY uses sigsetjmp, so changed local objects may be invalid after
     * a PostgreSQL ERROR. Keep preparation vectors and the saved C++ exception
     * in this buffer-owned object so the error handler can release them. */
    struct join_cpu_preparation_scratch final {
        std::vector<const bioseq::pg_types::seq*> query_headers{};
        std::vector<Datum> query_recipe_datums{};
        std::vector<pg_sequence_descriptor> query_recipe_descriptors{};
        std::vector<std::uint64_t> query_ordinals{};
        std::vector<bioseq::pg_types::seq*> detoast_copies{};
        std::vector<std::vector<char>> rewidthed_query_varlenas{};
        std::exception_ptr cpp_error{};

        [[nodiscard]] bool empty() const noexcept {
            return query_headers.empty() && query_recipe_datums.empty() &&
                   query_recipe_descriptors.empty() && query_ordinals.empty() &&
                   detoast_copies.empty() && rewidthed_query_varlenas.empty() &&
                   cpp_error == nullptr;
        }

        void clear_storage_noexcept() noexcept {
            std::vector<const bioseq::pg_types::seq*>{}.swap(query_headers);
            std::vector<Datum>{}.swap(query_recipe_datums);
            std::vector<pg_sequence_descriptor>{}.swap(query_recipe_descriptors);
            std::vector<std::uint64_t>{}.swap(query_ordinals);
            std::vector<bioseq::pg_types::seq*>{}.swap(detoast_copies);
            std::vector<std::vector<char>>{}.swap(rewidthed_query_varlenas);
            cpp_error = nullptr;
        }
    };

    struct join_output_cursor {
        std::size_t query_index = 0;
        std::size_t match_index = 0;
    };

  private:
    ::bioseq::simd::batch_processor processor;
    mmsearch_context mmsearch_state;

    /* The statement's fixed alignment plan. Empty for scans that  only provide sequences. */
    ::bioseq::simd::pipe::statement_execution_plan_owner execution_plan{};

    /*
     * Whether this scan may rzad stored minimizer indexes. The explicit index
     * requirement overrides the captured setting, and, the generic raw scalar
     * route disables these reads. Index access happens during dispatch.
     */
    bool read_sequence_index_active = false;
    bioseq::indexing::index_read_requirement index_read_requirement =
        bioseq::indexing::index_read_requirement::allow;

    /* Relation identity used to check row sources and open their stored indexes. */
    Oid user_relid = InvalidOid;

    /* Execution counters for the last batch, reported by EXPLAIN. */
    ::bioseq::pipeline_stats::pipeline_execution_stats last_stats{};

    std::vector<buffered_scan_entry> buffered_entries;
    ::bioseq::simd::match_result_map match_map;
    std::vector<output_cursor> output_cursors;
    int output_position = -1;

    bool join_mode = false;
    AttrNumber join_target_storage_seq_attno = InvalidAttrNumber;
    AttrNumber join_target_slot_seq_resno = InvalidAttrNumber;
    AttrNumber join_target_slot_rowno_resno = InvalidAttrNumber;
    AttrNumber join_query_slot_seq_resno = InvalidAttrNumber;
    std::vector<join_query_ref> join_queries{};
    /* Keep query slots until process_sequences selects the Highway CPU target.
     * Query sketches and index readers are prepared after that selection */
    std::vector<join_query_ref> join_pending_cpu_preparation_queries{};
    bool join_cpu_preparation_pending = false;
    join_cpu_preparation_scratch join_cpu_preparation_scratch{};
    ::bioseq::simd::basic_match_result_map<std::uint64_t> join_match_map{};
    std::vector<join_output_cursor> join_output_cursors{};
    std::size_t join_output_position = 0;
    bool join_target_active = false;
    bool join_target_prepared = false;
    bool join_target_outcomes_recorded = false;
    bool join_prepared_union_outcomes_recorded = false;
    bool join_prepared_build_checksum_recorded = false;
    std::optional<bioseq::storage::indexing::active_read_policy_resolution>
        join_active_read_policy{};
    bioseq::storage::indexing::cold_index_preparation_stats join_cold_policy_stats{};
    // Query views share ownership of this prepared target. Allocate a  new owner
    // for the next target so existing views keep their original data alive.
    std::shared_ptr<::bioseq::simd::batch_processor::cpu_prepared_target>
        join_cpu_prepared_target =
            std::make_shared<::bioseq::simd::batch_processor::cpu_prepared_target>();
    ::bioseq::simd::batch_processor::gpu_prepared_mpf_target
        join_gpu_prepared_mpf_target{};
    ::bioseq::simd::batch_processor::gpu_prepared_grouped_soa_target
        join_gpu_prepared_grouped_soa_target{};
    std::array<const bioseq::pg_types::seq*, dispatch_batch_capacity> join_target_hdrs{};
    std::array<const bioseq::pg_types::seq*, dispatch_batch_capacity> join_query_hdrs{};
    std::array<int32_t, dispatch_batch_capacity> join_query_ids{};
    std::array<bioseq::pg_types::seq*, dispatch_batch_capacity> join_query_detoast_copies{};
    std::vector<std::vector<char>> join_query_rewidth_varlenas{};
    std::unique_ptr<::bioseq::simd::batch_processor::scanner_pg_context> join_scanner_pg_ctx{};
    // Keep this cache pointer outside local storage so PG_CATCH can use it
    // after a longjmp.
    bioseq::storage::seq_caches* join_scanner_pg_cache = nullptr;
    ::bioseq::simd::match_result_map join_result_scratch{};

    /*
     * Extra constant queries filter the same target column. A row must match
     * every query to survive. Only the primary query supplies projected match
     * fields such as score and CIGAR. The caller checks that each extra query
     * has the target column's alphabet and storage width.
     */
    std::vector<std::vector<char>> extra_query_varlenas;

    /*
     * Storage identity and scratch  space for fetching locator payloads by
     * bucket whn a route needs complete sequences. Fetched payloads live
     * in batch_payload_ctx until the batch is cleared.
     */
    using storage_manager_type = bioseq::storage::bioseq_default_storage_manager;
    Oid user_main_database_oid = InvalidOid;
    Oid user_main_tablespace_oid = InvalidOid;
    Oid user_main_relfilenode = InvalidOid;
    std::optional<bioseq::storage::indexing::borrowed_executor_read_context>
        executor_read_context{};
    // The executor owns and registers this snapshot until all batches finish.
    // The buffer borrows it, and does not change its registration.
    Snapshot scan_snapshot = nullptr;
    MemoryContext executor_query_mctx = nullptr;
    // Borrowed from the scan. Locator-backed targets read through this provider,
    // which remains owned by the executor.
    bioseq::storage::pg_sequence_source_provider* source_provider = nullptr;
    // Borrowed from the statement's alignment_execution_collector.
    bioseq::pipeline_stats::persisted_gpu_operation_accumulator*
        persisted_gpu_operation_stats_sink = nullptr;
    bioseq::simd::downstream_active_wall_tracker*
        downstream_active_wall_tracker = nullptr;
    slice_async_owner_guard* slice_async_owner = nullptr;
    bioseq::storage::sequence_payload_allocation_observer mmsearch_target_payload_observer{};
    bioseq::storage::sequence_payload_allocation_observer mmsearch_query_payload_observer{};
    MemoryContext batch_payload_ctx = nullptr;
    /* Reuse this virtual slot while copying rows with expanded slices. Keeping
     * it  in the buffer lets cleanup find it after a PostgreSQL longjmp. */
    TupleTableSlot* copy_staging_slot = nullptr;
    std::array<std::vector<storage_manager_type::fetch_request>, storage_manager_type::bucket_count>
        per_bucket_target_requests{};
    std::array<std::vector<storage_manager_type::fetch_request>, storage_manager_type::bucket_count>
        per_bucket_query_requests{};
    std::vector<storage_manager_type::fetch_request> fetch_bulk_sort_scratch{};
    std::vector<bioseq::pg_types::seq*> hydrated_target_payloads{};
    std::vector<bioseq::pg_types::seq*> hydrated_query_payloads{};
    pg_sequence_read_session* active_wave_read_session = nullptr;
    // Keep the memory permit in the buffer so PG_CATCH can release it after
    // a longjmp. A changed local optional would have an indeterminate value
    std::optional<bioseq::parallelism::memory_permit> active_wave_permit{};
    /* Storage fetch counters for EXPLAIN. */
    std::uint64_t explain_bucket_touched_mask = 0;
    std::uint64_t explain_storage_bulk_fetch_calls = 0;

    struct evaluated_row_operands {
        Datum target = Datum{0};
        bool target_is_null = true;
        bool has_query_override = false;
        Datum query = Datum{0};
        bool query_is_null = true;
    };

    /** Physical relation and tuple identity supplied by a transformed target scan. */
    struct explicit_source_identity {
        Oid relation_oid = InvalidOid;
        ItemPointerData tid{};
    };

    void require_evaluated_mmsearch_index_policy_() const;

    static void require_registered_mvcc_snapshot_(Snapshot snapshot);

    [[nodiscard]] Datum retain_expanded_recipe_(Datum input);

    static void require_recipe_descriptor_(const pg_sequence_descriptor& descriptor,
                                           const std::uint8_t alphabet_code,
                                           const std::uint8_t storage_unit_byte_width,
                                           const char* operand);

    void capture_target_datum_(buffered_scan_entry& entry, Datum datum);

    void capture_query_datum_(buffered_scan_entry& entry, Datum datum);

    void retain_expanded_sequence_output_attributes_(buffered_scan_entry& entry,
                                                     TupleTableSlot* slot);

  public:
    explicit tuple_dispatch_buffer(const size_t estimated_tuple_count);

    /// Share the statement's GPU operation counters across batches and rescans.
    void bind_persisted_gpu_operation_stats_sink(
        bioseq::pipeline_stats::persisted_gpu_operation_accumulator& sink);

    /// Track downstream processing time in the statement's shared timer.
    void bind_downstream_active_wall_tracker(
        bioseq::simd::downstream_active_wall_tracker& tracker);

    void bind_slice_async_owner(slice_async_owner_guard* owner);

    void bind_sequence_source_provider(bioseq::storage::pg_sequence_source_provider* provider);

    void clear_sequence_source_provider_binding() noexcept;

    /**
     * Release the slice owqer after a per-row batch has been  fully drained.
     * A later rescan may get slices from a different projector. Leaf scan
     * buffers keep their owner across clear_slots() calls.
     */
    void release_per_row_slice_async_owner_after_drain();

    /* Keep the batch join's slice owner across all targets in one pass. Release
     * it after reset_join_runtime_for_rescan() has drained the pending work. */
    void release_join_slice_async_owner_after_drain();

    /*
     * Install the statement plan and storage context before submitting rows.
     * The plan already includes session settings and per-call options.
     * The executor must keep its relation, snapshot, and query context alive.
     */
    void configure_params(::bioseq::simd::pipe::statement_execution_plan_owner plan,
                          bioseq::indexing::index_read_requirement read_requirement,
                          bool read_sequence_index,
                          Oid scan_user_relid,
                          Oid scan_user_database_oid,
                          Oid scan_user_tablespace_oid,
                          Oid scan_user_main_relfilenode,
                          Relation executor_relation,
                          Snapshot executor_snapshot,
                          MemoryContext executor_query_context);

    // A parallel  worker restores the leader's snapshot after BeginCustomScan.
    // Replace the borrowed pointer before dispatch. scan_state still owns it.
    void replace_executor_snapshot(Snapshot executor_snapshot);

    [[nodiscard]] const ::bioseq::simd::minimap2_parameters& base_aligner_params() const {
        if (!execution_plan) {
            throw std::logic_error("alignment dispatch requires its statement plan");
        }
        return execution_plan->frozen_parameters;
    }
    [[nodiscard]] const ::bioseq::pipeline_stats::pipeline_execution_stats&
    execution_stats() const {
        return last_stats;
    }
    [[nodiscard]] bool reads_sequence_index() const {
        return read_sequence_index_active;
    }
    [[nodiscard]] AttrNumber target_sequence_attno() const noexcept {
        return mmsearch_state.seq_attno;
    }
    [[nodiscard]] AttrNumber per_row_query_sequence_attno() const noexcept {
        return mmsearch_state.per_row_query_attno;
    }

    /* Per-scan storage-bucket EXPLAIN counters. */
    [[nodiscard]] std::uint64_t explain_bucket_touched_bits() const {
        return explain_bucket_touched_mask;
    }
    [[nodiscard]] std::uint64_t explain_bulk_fetch_calls() const {
        return explain_storage_bulk_fetch_calls;
    }

    /*
     * Save the target column, primary-key column, and owned constant query.
     * Alignment uses the target alphabet and storage width. If the query
     * needs, conversion, the caller supplies it with apply_typmod_coalesce().
     */
    void configure_mmsearch(AttrNumber seq_attno,
                            std::string column_name,
                            std::vector<char> query_varlena_bytes,
                            uint8_t alphabet_code,
                            uint8_t storage_unit_byte_width,
                            AttrNumber primary_key_attno);

    /*
     * Use each row's query column for mmsearch. submit_scan saves the query
     * until batch execution. per_row_self_alignment describes identical
     * source columns for EXPLAIN, and still uses the normal aligner
     */
    void configure_mmsearch_per_row(AttrNumber seq_attno,
                                    std::string column_name,
                                    AttrNumber per_row_query_attno,
                                    bool per_row_self_alignment,
                                    uint8_t alphabet_code,
                                    uint8_t storage_unit_byte_width,
                                    uint8_t query_alphabet_code,
                                    uint8_t query_storage_unit_byte_width,
                                    AttrNumber primary_key_attno);

    /*
     * Accept  target and query xpression results with each submitted row.
     * seq_attno identifies the source and output column. The supplied
     * expression result is the alignment operand.
     */
    void configure_mmsearch_per_row_evaluated(const AttrNumber seq_attno,
                                              std::string column_name,
                                              const std::uint8_t alphabet_code,
                                              const std::uint8_t storage_unit_byte_width,
                                              const std::uint8_t query_alphabet_code,
                                              const std::uint8_t query_storage_unit_byte_width,
                                              const AttrNumber primary_key_attno);

    /* Configure a batch join with separate target and query child plans. */
    void configure_mmsearch_join(const Oid target_relation_oid,
                                 const AttrNumber target_storage_seq_attno,
                                 const AttrNumber target_slot_seq_resno,
                                 const AttrNumber target_slot_rowno_resno,
                                 const AttrNumber query_slot_seq_resno,
                                 std::string target_column_name,
                                 const std::uint8_t alphabet_code,
                                 const std::uint8_t storage_unit_byte_width,
                                 const std::uint8_t query_alphabet_code,
                                 const std::uint8_t query_storage_unit_byte_width);

    /* Keep one target row for reuse by every query batch in the join. */
    void begin_join_target(TupleTableSlot* target_slot);

  private:
    static void prepare_join_cpu_after_highway_selection_thunk(
        void* state,
        ::bioseq::simd::batch_processor::scanner_pg_context& scanner_pg_ctx,
        ::bioseq::simd::pipe::statement_execution_plan_owner dispatched_plan,
        const ::bioseq::simd::batch_processor::cpu_preparation_typmod_operations& typmod_ops);

    void prepare_join_cpu_after_highway_selection(
        ::bioseq::simd::batch_processor::scanner_pg_context& scanner_pg_ctx,
        ::bioseq::simd::pipe::statement_execution_plan_owner dispatched_plan,
        const ::bioseq::simd::batch_processor::cpu_preparation_typmod_operations& typmod_ops);

  public:
    void prepare_join_target_queries(std::span<const join_query_ref> all_queries);

    void set_join_query_batch(std::span<const join_query_ref> queries);

    /*
     * Store a query already converted to the target alphabet and storage
     * width. Keep the original bytes for EXPLAIN. query_hdr() returns the
     * converted value while coalesce_active is set.
     */
    void apply_typmod_coalesce(std::vector<char> coalesced_bytes);

    [[nodiscard]] bool per_row_mode() const noexcept;
    [[nodiscard]] AttrNumber per_row_query_attno() const;
    [[nodiscard]] AttrNumber query_source_attno() const noexcept;
    [[nodiscard]] bool coalesce_active() const;
    [[nodiscard]] bool per_row_self_alignment() const;
    [[nodiscard]] uint8_t mmsearch_alphabet_code() const;
    [[nodiscard]] uint8_t mmsearch_storage_unit_byte_width() const;

    /*
     * Reset the primary mmsearch query. Preserve extra  constant filters by
     * default so a NULL parameter on one nested-loop pass does not remove
     * those filters from later rescans. Pass clear_extras=true for a full reset.
     */
    void clear_mmsearch_context(bool clear_extras = false);

    /*
     * Save another constant query as a filter on the primary target column.
     * The caller must check the column, alphabet, and storage width before
     * adding it. This method takes cwnership of the supplied bytes.
     */
    void add_extra_const_query(std::vector<char> query_varlena_bytes);

    [[nodiscard]] size_t extra_query_count() const;

    /*
     * Save a row that passed the scan filter. Copy its tuple and capture
     * the alignment operands before the scan reuses its slot. Locators and
     * expanded slices keep their deferred representation.
     */
    [[gnu::always_inline]] inline void submit_scan(TupleTableSlot* scan_slot) {
        submit_scan_impl_(scan_slot, nullptr, nullptr, /*accept_slot_tid=*/true);
    }

    void submit_scan_with_source_tid(TupleTableSlot* scan_slot,
                                     const Oid source_relation_oid,
                                     const ItemPointerData source_tid) {
        require_evaluated_mmsearch_index_policy_();
        if (!OidIsValid(source_relation_oid) || !ItemPointerIsValid(&source_tid)) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("bioseq transformed target source identity is invalid")));
        }
        const explicit_source_identity source_identity{
            .relation_oid = source_relation_oid,
            .tid = source_tid,
        };
        submit_scan_impl_(scan_slot,
                          nullptr,
                          std::addressof(source_identity),
                          /*accept_slot_tid=*/false);
    }

    void submit_scan_recipe_without_source_tid(TupleTableSlot* scan_slot) {
        require_evaluated_mmsearch_index_policy_();
        submit_scan_impl_(scan_slot, nullptr, nullptr, /*accept_slot_tid=*/false);
    }

    void submit_scan_evaluated(TupleTableSlot* scan_slot,
                               const Datum target,
                               const bool target_is_null,
                               const bool has_query_override,
                               const Datum query,
                               const bool query_is_null) {
        require_evaluated_mmsearch_index_policy_();
        if (has_query_override != per_row_mode()) {
            ereport(
                ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg(
                     "evaluated bioseq mmsearch query submission disagrees with its configuration"),
                 errdetail("per_row_mode=%s has_query_override=%s",
                           per_row_mode() ? "true" : "false",
                           has_query_override ? "true" : "false")));
        }
        const evaluated_row_operands evaluated{
            .target = target,
            .target_is_null = target_is_null,
            .has_query_override = has_query_override,
            .query = query,
            .query_is_null = query_is_null,
        };
        submit_scan_impl_(scan_slot,
                          std::addressof(evaluated),
                          nullptr,
                          /*accept_slot_tid=*/true);
    }

  private:
    [[gnu::always_inline]] inline void submit_scan_impl_(TupleTableSlot* scan_slot,
                                                         const evaluated_row_operands* evaluated,
                                                         const explicit_source_identity*
                                                             source_identity,
                                                         const bool accept_slot_tid) {
        if (!mmsearch_state.active) {
            bioseq_log<logging_level::debug>(
                "[bioseq] pg: submit_scan called while mmsearch state inactive; skipping");
            return;
        }
        if (scan_slot == nullptr) {
            bioseq_log<logging_level::debug>(
                "[bioseq] pg: submit_scan received null scan_slot; skipping");
            return;
        }
        if (buffered_entries.size() >= dispatch_batch_capacity) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("bioseq dispatch buffer capacity invariant was violated"),
                     errdetail("buffered=%zu capacity=%zu",
                               buffered_entries.size(),
                               dispatch_batch_capacity)));
        }

        buffered_scan_entry entry{};

        /*
         * A transformed row supplies tableoid and ctid separately. Check  them
         * against the slot and configured relation before assigning a row
         * number Rows from other relations keep -1, so routes requiring
         * direct physical identity, cannot use them.
         */
        if (source_identity != nullptr) {
            if (!OidIsValid(source_identity->relation_oid) ||
                !ItemPointerIsValid(&source_identity->tid)) {
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("bioseq transformed target source identity is invalid")));
            }
            if (OidIsValid(scan_slot->tts_tableOid) &&
                scan_slot->tts_tableOid != source_identity->relation_oid) {
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("bioseq transformed target table identity disagrees with its slot")));
            }
            if (ItemPointerIsValid(&scan_slot->tts_tid) &&
                (ItemPointerGetBlockNumberNoCheck(&scan_slot->tts_tid) !=
                     ItemPointerGetBlockNumberNoCheck(&source_identity->tid) ||
                 ItemPointerGetOffsetNumberNoCheck(&scan_slot->tts_tid) !=
                     ItemPointerGetOffsetNumberNoCheck(&source_identity->tid))) {
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("bioseq transformed target TID disagrees with its slot")));
            }
            if (source_identity->relation_oid == user_relid) {
                entry.sequence_rowno =
                    static_cast<int64_t>(tid_to_row_number(source_identity->tid));
            }
        } else if (accept_slot_tid && ItemPointerIsValid(&scan_slot->tts_tid)) {
            /* Direct scans provide their row identity in the slot. */
            entry.sequence_rowno = static_cast<int64_t>(tid_to_row_number(scan_slot->tts_tid));
        }

        /* The expression result determines NULLness. Its source column may have
         * a different NULL value, so use the supplied operand directly. */
        if (evaluated != nullptr) {
            entry.target_from_evaluated_expression = true;
            if (evaluated->target_is_null) {
                entry.seq_is_null = true;
            } else if (evaluated->target == Datum{0}) {
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("evaluated bioseq mmsearch target has a null Datum")));
            } else {
                capture_target_datum_(entry, evaluated->target);
            }
        } else if (mmsearch_state.seq_attno == InvalidAttrNumber) {
            entry.seq_is_null = true;
        } else {
            bool is_null = true;
            Datum seq_datum = slot_getattr(scan_slot, mmsearch_state.seq_attno, &is_null);
            if (is_null) {
                entry.seq_is_null = true;
            } else {
                /* Capture inline bytes, a locator, or an expanded slice as appropriate.
                 * Flattening an expanded slice here would read its full sequence
                 * before the  alignment needs it. */
                capture_target_datum_(entry, seq_datum);
            }
        }

        /* Extract the primary-key column. */
        if (mmsearch_state.primary_key_attno == InvalidAttrNumber) {
            entry.primary_key_is_null = true;
        } else {
            bool pk_null = true;
            Datum pk_datum = slot_getattr(scan_slot, mmsearch_state.primary_key_attno, &pk_null);
            if (pk_null) {
                entry.primary_key_is_null = true;
            } else {
                entry.primary_key = DatumGetInt32(pk_datum);
                entry.primary_key_is_null = false;
            }
        }

        /*
         * Save this row's query operand. The private flat copy may be
         * converted to the target storage width. Output keeps the original
         * column value, including its declared width.
         */
        if (evaluated != nullptr && evaluated->has_query_override) {
            entry.query_from_evaluated_expression = true;
            if (evaluated->query_is_null) {
                entry.per_row_query_is_null = true;
            } else if (evaluated->query == Datum{0}) {
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("evaluated bioseq mmsearch query has a null Datum")));
            } else {
                /* Save the query expression separately. Two slices of the same source
                 * coluin can describe different sequences. */
                capture_query_datum_(entry, evaluated->query);
            }
        } else if (mmsearch_state.per_row_query_attno != InvalidAttrNumber) {
            bool query_null = true;
            Datum query_datum =
                slot_getattr(scan_slot, mmsearch_state.per_row_query_attno, &query_null);
            if (query_null) {
                entry.per_row_query_is_null = true;
            } else if (mmsearch_state.per_row_query_attno == mmsearch_state.seq_attno &&
                       entry.has_seq_recipe()) {
                /* Both operands name the same untransformed column, so share its wrapper. */
                require_recipe_descriptor_(entry.seq_recipe_descriptor,
                                           mmsearch_state.query_alphabet_code,
                                           mmsearch_state.query_storage_unit_byte_width,
                                           "query");
                entry.per_row_query_recipe_datum = entry.seq_recipe_datum;
                entry.per_row_query_recipe_descriptor = entry.seq_recipe_descriptor;
                entry.per_row_query_is_null = false;
            } else {
                capture_query_datum_(entry, query_datum);
            }
        }

        /*
         * Populate every slot attribute before copying the tuple. The  column
         * scan may have filled only requested attributes, while MinimalTuple
         * serialization reads the full slot.
         */
        slot_getallattrs(scan_slot);
        retain_expanded_sequence_output_attributes_(entry, scan_slot);

        /*
         * Hide saved slice wrappers in a staging slot while copying the
         * tuple, then restore them in the result. ExecCopySlotMinimalTuple
         * would otherwise flatten them. Leave the input slot untouched
         */
        TupleTableSlot* copy_source = scan_slot;
        if (batch_payload_ctx == nullptr) {
            ereport(ERROR,
                    (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                     errmsg("bioseq dispatch tuple arena is not initialized")));
        }
        MemoryContext previous_copy_context = MemoryContextSwitchTo(batch_payload_ctx);
        if (!entry.expanded_sequence_attributes.empty()) {
            if (copy_staging_slot == nullptr) {
                copy_staging_slot =
                    MakeSingleTupleTableSlot(scan_slot->tts_tupleDescriptor, &TTSOpsVirtual);
            } else if (copy_staging_slot->tts_tupleDescriptor != scan_slot->tts_tupleDescriptor) {
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("bioseq dispatch staging slot descriptor changed")));
            }
            ExecClearTuple(copy_staging_slot);
            const int natts = scan_slot->tts_tupleDescriptor->natts;
            std::memcpy(copy_staging_slot->tts_values,
                        scan_slot->tts_values,
                        static_cast<std::size_t>(natts) * sizeof(Datum));
            std::memcpy(copy_staging_slot->tts_isnull,
                        scan_slot->tts_isnull,
                        static_cast<std::size_t>(natts) * sizeof(bool));
            const auto mask_recipe = [this, natts](AttrNumber attno) {
                const int index = static_cast<int>(attno) - 1;
                if (index >= 0 && index < natts) {
                    copy_staging_slot->tts_values[index] = Datum{0};
                    copy_staging_slot->tts_isnull[index] = true;
                }
            };
            for (const auto& expanded : entry.expanded_sequence_attributes) {
                mask_recipe(expanded.attno);
            }
            copy_staging_slot->tts_nvalid = natts;
            ExecStoreVirtualTuple(copy_staging_slot);
            copy_source = copy_staging_slot;
        }

        entry.scan_slot_copy =
            MakeSingleTupleTableSlot(scan_slot->tts_tupleDescriptor, &TTSOpsMinimalTuple);
        ExecStoreMinimalTuple(
            ExecCopySlotMinimalTuple(copy_source), entry.scan_slot_copy, /*shouldFree=*/true);
        /* Copy system-column identity separately because MinimalTuple stores
         * only ordinary attributes. Only direct scans keep the slot TID. */
        if (accept_slot_tid) {
            entry.scan_slot_copy->tts_tid = scan_slot->tts_tid;
            entry.scan_slot_copy->tts_tableOid = scan_slot->tts_tableOid;
        }
        MemoryContextSwitchTo(previous_copy_context);

        /* Populate the copied slot before restoring saved slices and using
         * it for output projection. */
        slot_getallattrs(entry.scan_slot_copy);
        bioseq::postgres::support::pg_slot_datums_view copied_values{entry.scan_slot_copy};
        for (const auto& expanded : entry.expanded_sequence_attributes) {
            copied_values.set_raw(expanded.attno,
                                  bioseq::postgres::support::pg_datum::from_raw(expanded.datum));
        }

        /* Stored indexes are read during batch,  execution. Keep the source row
         * number here so the scanner can find the correct index later. */
        const std::uint64_t entry_seq_bp_size = entry.seq_bp_size();
        bioseq_log<logging_level::debug>(
            "[bioseq] pg: submit_scan pk={} rowno={} seq_bp_size={} (null={})",
            entry.primary_key_is_null ? -1 : entry.primary_key,
            entry.sequence_rowno,
            entry_seq_bp_size,
            entry.seq_is_null);

        buffered_entries.push_back(std::move(entry));
    }

  public:
    /* True when the current batch has reached its row limit. */
    [[nodiscard]] bool ready_to_dispatch() const noexcept {
        return buffered_entries.size() == dispatch_batch_capacity;
    }

    /*
     * Fetch complete sequences for the requested rows, grouped by storage
     * ucket. Keep payloads in the batch arena and update the dispatch
     * pointers. Raw sequence columns in copied output slots also receive
     * the payload. Evaluated expression operands leave those columns intact.
     * Sequences exceeding MaxAllocSize stay  deferred.
     */
    void hydrate_one_column(std::span<const size_t> rows,
                            bioseq::storage::seq_caches* cache,
                            std::int16_t column_attno,
                            std::array<std::vector<storage_manager_type::fetch_request>,
                                       storage_manager_type::bucket_count>& per_bucket,
                            std::vector<bioseq::pg_types::seq*>& out_payloads,
                            bool is_query);

    /* Fetch complete payloads for the in-memory and synthetic-scanner tests.
     * Production dispatch reads through the source provider. */
    void hydrate_locators_by_bucket(std::span<const size_t> rows);

    [[nodiscard]] bool run_join_query_batch_and_build_cursors();

    void finish_join_target();

    /// Discard join state after an error and abort any active sequence reads.
    /// Leave tuple and cache handles for PostgreSQL error cleanup while
    /// releasing C++ storage and memory permits.
    void abandon_join_runtime_after_pg_error_noexcept() noexcept;

    void reset_join_runtime_for_rescan();

    /*
     * Align a full batch or the rows left at end of input. Build one output
     * cursor per surviving match in submission order. The processor selects
     * the  implementation for the target alphabet and storage width
     */
    [[nodiscard]] bool run_batch_and_build_cursors();

    void run_batch_and_build_cursors_impl();

    /*
     * Return the next copied row and match, or nullopt when output is exhausted.
     * Raise a PostgreSQL error if a cursor points to missing data.
     */
    [[nodiscard]] std::optional<next_output> read_next_output();

    /* True while this batch is ready for the custom scan to drain its results. */
    [[nodiscard]] constexpr bool has_finished_writing() const {
        return output_position >= 0;
    }

    /*
     * True when mmsearch is configured. A scan without mmsearch projects its
     * input rows directly and does not need this alignment buffe.
     */
    [[nodiscard]] constexpr bool mmsearch_active() const {
        return mmsearch_state.active;
    }

    [[nodiscard]] size_t buffered_count() const {
        return buffered_entries.size();
    }

    [[nodiscard]] size_t output_count() const {
        return output_cursors.size();
    }

    /*
     * Release the completed batch and prepare to collect, more rows. Keep the
     * mmsearch configuration so the next  batch or rescan can reuse it.
     */
    [[gnu::always_inline]] inline void clear_slots() {
        bioseq_log<logging_level::debug>(
            "[bioseq] pg: tuple_dispatch_buffer::clear_slots() entered; freeing {} slots, {} outputs",
            buffered_entries.size(),
            output_cursors.size());
        for (auto& entry : buffered_entries) {
            if (entry.scan_slot_copy != nullptr) {
                ExecDropSingleTupleTableSlot(entry.scan_slot_copy);
                entry.scan_slot_copy = nullptr;
            }
        }
        buffered_entries.clear();
        match_map.clear();
        output_cursors.clear();
        output_position = -1;
        ::abort_pg_sequence_read_session_noexcept(std::exchange(active_wave_read_session, nullptr));
        active_wave_permit.reset();
        if (copy_staging_slot != nullptr) {
            ExecDropSingleTupleTableSlot(copy_staging_slot);
            copy_staging_slot = nullptr;
        }
        /* All output has been drained and row references are gone, so the
         * batch's fetched payloads and saved slice wrappers can be freed. */
        if (batch_payload_ctx != nullptr) {
            MemoryContextReset(batch_payload_ctx);
        }
    }

    /* Release the batch and reset mmsearch, including extra constant queries. */
    [[gnu::always_inline]] inline void clear() {
        clear_slots();
        clear_mmsearch_context(/*clear_extras=*/true);
    }
};

static_assert(noexcept(
    std::declval<tuple_dispatch_buffer&>().abandon_join_runtime_after_pg_error_noexcept()));

} // namespace bioseq::scheduler
