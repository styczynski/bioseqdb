/**
 * @file
 * @brief Implements buffered sequence dispatch for custom scans.
 */
/*-------------------------------------------------------------------------
 *
 * scheduler/dispatch_buffer/index.cppm
 *
 * tuple_dispatch_buffer accumulates TupleTableSlot copies produced during
 * a custom-scan execution stage so the scan node can later replay them in
 * order. The scan runs in two stages per query:
 *
 *   Writing stage (driven by submit_scan): for every source tuple that
 *   passed the WHERE qual, the scan copies the slot into the buffer and
 *   records the row's primary key and a detoasted copy of its bioseq.seq
 *   varlena. Projection does NOT happen here; `ecxt_scantuple` still
 *   references the original scan slot which is about to be overwritten
 *   by the next call.
 *
 *   Batch stage (run_batch_and_build_cursors): after the access method
 *   signals EOF, the buffer feeds its target varlenas (plus the mmsearch
 *   query varlena captured at BeginCustomScan) into
 *   batch_processor::process_sequences. That function dispatches on the
 *   column's (alphabet_code, storage_unit_byte_width) typmod to instantiate the right
 *   compact_sequence_range<p_sequence> specialisation, then invokes
 *   minimap_port(query, target) for every row and stores the returned
 *   match results keyed by primary key. Rows whose PK is missing from the
 *   returned map are filtered; this is how WHERE mmsearch(...) evaluates
 *   to FALSE for those rows.
 *
 *   Reading stage (read_next_output): the custom scan iterates over
 *   output cursors, pointing `ecxt_scantuple` at the buffered scan slot
 *   and projecting via ExecProject. The placeholder Const* nodes
 *   injected by variable injection mutator are updated from the match result
 *   before projection fires.
 *
 *-------------------------------------------------------------------------
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

// The chunked-scanner dispatch path is always compiled as a permanent part of
// the fragmented pipeline tree.
// The `bioseq.testing.chunked_scanner.enabled` GUC routes row sequences through a
// synthetic chunked scanner end-to-end. The
// `bioseq.testing.in_memory_scanner.enabled` GUC forces the materialise-then-
// in_memory baseline. Without either GUC the production path promotes locator
// values through the statement-owned source provider and runs from its
// canonical lazy facade.
export module bioseq.postgres.scheduler.dispatch_buffer;

export import :accounting;

import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.postgres.support;
import bioseq.postgres.support.datums;
import bioseq.postgres.configuration.core;
import bioseq.indexing.type;
import bioseq.postgres.storage.indexing.handles;
import bioseq.postgres.storage.indexing.chunked_storage;
import bioseq.storage.lazy_sequence;                  // allocation observer for route-aware sequence payload tracing
import bioseq.postgres.scheduler.dispatch_processor;   // fixed-shape batch_processor, heavy pipeline in its own module
import bioseq.sequences;
import bioseq.sequences.simd;
import bioseq.pipeline.minimap_port;
import bioseq.sequences.cigar;
import bioseq.pipeline.pipe;
import bioseq.postgres.types.compact_sequence;
import bioseq.postgres.storage.manager;
import bioseq.storage.scanner;
import bioseq.storage.layout;
import bioseq.postgres.storage.scanner;       // statement-owned source provider for locator targets
import bioseq.parallelism.memory_admission;
import bioseq.postgres.scheduler.supervisor;
import bioseq.postgres.column.table_access;

export namespace bioseq::scheduler {


// Fixed upper bound on one physical dispatch. The custom scan rolls over at
// this boundary, drains every result, clears the completed physical batch and
// then resumes the same source scan. No source tuple may be truncated.
class tuple_dispatch_buffer {
public:
    /*
     * Context captured at BeginCustomScan from cscan->scan.plan.qual /
     * targetlist. seq_attno is the 1-based attribute number of the Var
     * passed as the first argument of mmsearch(); primary_key_attno is
     * the 1-based attribute number of the Var passed as the argument of
     * any bioseq.<field>(...) projection function. alphabet_code /
     * storage_unit_byte_width hold the (alphabet, width) typmod shared by the column
     * and the query literal — they are validated to be equal at
     * BeginCustomScan time. query_varlena holds a full detoasted copy
     * of the mmsearch second-argument bioseq.seq Const, so submit_scan
     * never has to re-read from the plan tree.
     */
    struct mmsearch_context {
        AttrNumber seq_attno           = InvalidAttrNumber;
        AttrNumber primary_key_attno   = InvalidAttrNumber;
        std::string column_name;
        std::vector<char> query_varlena;  // full detoasted varlena bytes, 16-byte aligned by std::allocator
        uint8_t alphabet_code          = 0;
        uint8_t storage_unit_byte_width             = 0;
        uint8_t query_alphabet_code    = 0;
        uint8_t query_storage_unit_byte_width = 0;
        bool active                    = false;

        /*
         * Per-row-query attno. Set when the mmsearch second argument is a
         * Var of the SAME relation (e.g. `mmsearch(t1.seq, t1.seq)` for
         * self-alignment, or `mmsearch(t1.seq, t1.seq2)` for sibling-
         * column alignment). The planner does not parameterize same-rel
         * Vars (no Param injection happens) so the existing Const/Param
         * paths reject the qual; instead we extract the value of this
         * column at submit_scan time and store it per-row in
         * buffered_scan_entry::per_row_query_varlena. Run-batch then
         * dispatches process_sequences once per entry against that
         * row's own query — which is the only correct semantics for
         * per-row mmsearch (broadcasting one row's query to other rows
         * would incorrectly reuse one row's query for other rows).
         *
         * `per_row_self_alignment` is the special case where the
         * arg1 Var has the SAME varattno as the arg0 Var (same column).
         * mmsearch(seq, seq) is then equivalent to "every row aligns
         * against itself" — guaranteed full-length match. We still
         * dispatch through the aligner so the produced match result is
         * the byte-for-byte alignment that the user can inspect via
         * bioseq.cigar / target_start / etc.
         */
        AttrNumber per_row_query_attno      = InvalidAttrNumber;
        bool       per_row_self_alignment   = false;
        bool       per_row_evaluated        = false;

        /*
         * Typmod coalesce: when the column and the query have the same
         * alphabet but different widths (e.g. column is dna4/32 and
         * query is dna4/64), the bytes in the query header carry the
         * larger width. The bioseq pipeline templates on
         * (alphabet_code, storage_unit_byte_width) and dispatches via
         * bioseq::pg_types::seq::dispatch_instance, so the query and target MUST
         * agree on both.
         * `coalesced_query_varlena` holds the query bytes re-encoded
         * at the COLUMN's (alphabet, width). The non-coalesced
         * `query_varlena` is preserved for the EXPLAIN line so users
         * see what the original literal was; the dispatch helper
         * `effective_query_hdr()` returns the coalesced bytes when
         * available, and the original ones otherwise.
         */
        std::vector<char> coalesced_query_varlena;
        bool       coalesce_active          = false;
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
     * One buffered scan tuple. The slot copy holds the full base-relation
     * tuple so projection can resolve any Var reference at read time.
     * primary_key is the detoasted INT4 value of the PK column; it is
     * used as the map key by the batch processor. seq_varlena is a
     * detoasted copy of the mmsearch target column's bioseq.seq value;
     * it is passed through batch_processor::process_sequences as the
     * per-row target sequence. Copying rather than pointing into the
     * slot is mandatory: tts_values[] is reused across scan calls and
     * the TOAST-detoasted buffer lives in CurrentMemoryContext which
     * may reset between rows.
     */
    struct buffered_scan_entry {
        struct retained_expanded_attribute {
            AttrNumber attno = InvalidAttrNumber;
            Datum datum = Datum{0};
            pg_sequence_descriptor descriptor{};
        };
        TupleTableSlot* scan_slot_copy      = nullptr;
        int32_t         primary_key         = -1;
        bool            primary_key_is_null = true;
        int64_t         sequence_rowno      = -1;   /* derived from scan_slot->tts_tid; -1 when the TID was invalid */
        std::vector<char> seq_varlena;
        bool            seq_is_null         = true;
        bool            target_from_evaluated_expression = false;

        /*
         * Per-row mmsearch query, populated only when mmsearch state's
         * `per_row_query_attno` is set (i.e. the qual is
         * `mmsearch(this_rel.seq_a, this_rel.seq_b)` where the second
         * arg is a Var of the same relation). Holds the detoasted
         * bioseq.seq varlena bytes for THIS row's `seq_b` column — so
         * each row aligns against its own query. The bytes are stored
         * in the column's typmod (no coalesce needed: arg1 must be a
         * column of the same relation, which means it shares the
         * column's alphabet/width via the same Var resolution path).
         */
        std::vector<char> per_row_query_varlena;
        bool            per_row_query_is_null = true;
        bool            query_from_evaluated_expression = false;

        /*
         * Deferred-hydration stage. Under
         * defer_seq_hydration the custom scan returns 40-byte locator stubs
         * instead of materialised payloads, so submit_scan records the
         * decoded locator here (leaving seq_varlena empty). Production
         * CPU/SIMD dispatch preserves that locator until the algorithm enters
         * the statement source-provider facade. The hydrated pointers remain
         * only for explicit flat baselines and Vulkan's packed-upload ABI.
         * The seq_varlena path is still the fast path for a small inline value.
         */
        std::optional<bioseq::storage::sequence_locator> seq_loc;
        std::optional<bioseq::storage::sequence_locator> per_row_query_loc;
        /*
         * A slice projection result is an expanded piece-tree Datum, not a flat
         * bioseq::pg_types::seq.  Keep a tiny read-only wrapper in the batch arena instead
         * of asking PG_DETOAST_DATUM to invoke the EOH flatten callback while
         * the row is merely being admitted.  The copied wrapper retains every
         * source-domain edge and remains valid until clear_slots() resets the
         * arena after the output cursors have drained.
         */
        Datum seq_recipe_datum = Datum{0};
        Datum per_row_query_recipe_datum = Datum{0};
        pg_sequence_descriptor seq_recipe_descriptor{};
        pg_sequence_descriptor per_row_query_recipe_descriptor{};
        std::vector<retained_expanded_attribute> expanded_sequence_attributes{};
        const bioseq::pg_types::seq* hydrated_seq_hdr           = nullptr;
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
            return reinterpret_cast<const bioseq::pg_types::seq*>(
                per_row_query_varlena.data());
        }
    };

    /*
     * A cursor into (buffered_entries, match_map). output_cursors is
     * built in insertion order so EXPLAIN output mirrors INSERT order.
     */
    struct output_cursor {
        size_t scan_entry_index = 0;
        size_t match_index      = 0;
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

	/* PG_TRY is sigsetjmp-based, so mutable automatic C++ objects become
	 * indeterminate after an ERROR longjmp.  Deferred Highway preparation keeps
	 * every vector and exception receipt in this stable buffer-owned object;
	 * only trivial scalar locals cross the inner PG_TRY boundary. */
	struct join_cpu_preparation_scratch final {
		std::vector<const bioseq::pg_types::seq*> query_headers{};
		std::vector<Datum> query_recipe_datums{};
		std::vector<pg_sequence_descriptor> query_recipe_descriptors{};
		std::vector<std::uint64_t> query_ordinals{};
		std::vector<bioseq::pg_types::seq*> detoast_copies{};
		std::vector<std::vector<char>> rewidthed_query_varlenas{};
		std::exception_ptr cpp_error{};

		[[nodiscard]] bool empty() const noexcept {
			return query_headers.empty()
				&& query_recipe_datums.empty()
				&& query_recipe_descriptors.empty()
				&& query_ordinals.empty()
				&& detoast_copies.empty()
				&& rewidthed_query_varlenas.empty()
				&& cpp_error == nullptr;
		}

		void clear_storage_noexcept() noexcept {
			std::vector<const bioseq::pg_types::seq*>{}.swap(query_headers);
			std::vector<Datum>{}.swap(query_recipe_datums);
			std::vector<pg_sequence_descriptor>{}.swap(
				query_recipe_descriptors);
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

    /*
     * Per-scan base minimap2_parameters, captured when the column scan begins
     * from the plan (GUC snapshot + per-call aligner_options() overrides) and
     * pushed into batch_processor::process_sequences once per batch. Every
     * row inherits these values with size-heuristic tuning applied if the
     * user left the minimap2 defaults in place.
     */
    ::bioseq::simd::minimap2_parameters base_params{};

    /*
     * Effective per-scan persisted-payload read gate. `allow` preserves the
     * captured bioseq.index.sequence_read.enabled value; the two explicit contracts
     * override it. Payload probes happen later in the typed scanner path,
     * never in submit_scan.
     */
    bool read_sequence_index_active = false;
    bioseq::indexing::index_read_requirement index_read_requirement =
        bioseq::indexing::index_read_requirement::allow;

    /*
     * The OID of the relation being scanned. Only used to key into
     * bioseq_internal.index_sidecar; set at configure time so submit_scan
     * doesn't need to re-read RelationGetRelid per tuple.
     */
    Oid user_relid = InvalidOid;

    /*
     * Execution statistics captured during the last batch. Used by
     * the column-scan explain callback so it can compute the "Bioseq
     * Recomputed" percentage from runtime truth instead of the
     * plan-time estimate.
     */
    ::bioseq::pipeline_stats::pipeline_execution_stats last_stats{};

    std::vector<buffered_scan_entry> buffered_entries;
    std::map<int32_t, std::vector<::bioseq::simd::match_result>> match_map;
    std::vector<output_cursor> output_cursors;
    int output_position = -1;

    bool join_mode = false;
    AttrNumber join_target_storage_seq_attno = InvalidAttrNumber;
    AttrNumber join_target_slot_seq_resno = InvalidAttrNumber;
    AttrNumber join_target_slot_rowno_resno = InvalidAttrNumber;
    AttrNumber join_query_slot_seq_resno = InvalidAttrNumber;
    std::vector<join_query_ref> join_queries{};
	/* Stable query-slot references for CPU preparation.  Keeping only these
	 * scalar descriptors before dispatch ensures that every Highway-sensitive
	 * sketch/reader action runs below process_sequences' one selector. */
	std::vector<join_query_ref> join_pending_cpu_preparation_queries{};
	bool join_cpu_preparation_pending = false;
	join_cpu_preparation_scratch join_cpu_preparation_scratch{};
    std::map<std::uint64_t,
        std::vector<::bioseq::simd::match_result>>
        join_match_map{};
    std::vector<join_output_cursor> join_output_cursors{};
    std::size_t join_output_position = 0;
    bool join_target_active = false;
    bool join_target_prepared = false;
    bool join_target_outcomes_recorded = false;
	bool join_prepared_union_outcomes_recorded = false;
	bool join_prepared_build_checksum_recorded = false;
	std::optional<bioseq::storage::indexing::active_read_policy_resolution>
		join_active_read_policy{};
	bioseq::storage::indexing::cold_index_preparation_stats
		join_cold_policy_stats{};
	::bioseq::simd::batch_processor::cpu_prepared_target
		join_cpu_prepared_target{};
	::bioseq::simd::batch_processor::gpu_prepared_grouped_soa_target
		join_gpu_prepared_grouped_soa_target{};
    std::array<const bioseq::pg_types::seq*, dispatch_batch_capacity> join_target_hdrs{};
    std::array<const bioseq::pg_types::seq*, dispatch_batch_capacity> join_query_hdrs{};
    std::array<int32_t, dispatch_batch_capacity> join_query_ids{};
    std::array<bioseq::pg_types::seq*, dispatch_batch_capacity> join_query_detoast_copies{};
    std::vector<std::vector<char>> join_query_rewidth_varlenas{};
    std::unique_ptr<
        ::bioseq::simd::batch_processor::scanner_pg_context>
        join_scanner_pg_ctx{};
    // PG_TRY is sigsetjmp-based: the catch path must not read a cache pointer
    // automatic that was assigned inside the protected region.
    bioseq::storage::seq_caches* join_scanner_pg_cache = nullptr;
    std::map<int32_t,
        std::vector<::bioseq::simd::match_result>>
        join_result_scratch{};

    /*
     * Additional Const-only mmsearch quals that share the primary
     * mmsearch state's seq column and (alphabet, width) typmod.
     * Each entry holds the detoasted bioseq.seq varlena bytes for one
     * extra query. After the primary batch runs, run_batch_and_build_cursors
     * dispatches process_sequences once per extra query and intersects
     * the per-entry match presence so output_cursors only emit rows
     * that match every mmsearch qual (logical AND).
     *
     * Only the FIRST mmsearch context drives projection placeholders
     * (bioseq.cigar / score / target_*); the extra queries are pure
     * filters. Param-driven (cross-table) extras and same-column-but-
     * different-typmod extras are not supported in this iteration —
     * they are detected and rejected at BeginCustomScan with a log
     * line so the user sees why the qual didn't apply.
     */
    std::vector<std::vector<char>> extra_query_varlenas;

    /*
     * Per-bucket deferred-hydration machinery. The custom scan
     * runs with defer_seq_hydration=true, so submit_scan captures only
     * locators; run_batch_and_build_cursors groups the buffered locators by
     * bucket_index and issues one fetch_payload_bulk per non-empty bucket
     * (each opens that bucket's shards once, scanning them row-number-sorted).
     * Hydrated payloads live in batch_payload_ctx (reset per batch); the
     * per-scan db/spc/relfilenode identity is captured by configure_params.
     */
    using storage_manager_type = bioseq::storage::bioseq_default_storage_manager;
    Oid user_main_database_oid   = InvalidOid;
    Oid user_main_tablespace_oid = InvalidOid;
    Oid user_main_relfilenode    = InvalidOid;
    std::optional<bioseq::storage::indexing::borrowed_executor_read_context>
        executor_read_context{};
    // Borrowed from the owning executor state for active scan execution. The
    // owner keeps it registered until every batch/scanner is drained; buffer
    // destruction never dereferences it and never changes its registration.
    Snapshot scan_snapshot       = nullptr;
    MemoryContext executor_query_mctx = nullptr;
    // Borrowed from the bioseq scan provider. Every locator-backed scanner
    // target must use this exact source-domain route; this buffer never owns
    // or closes it.
    bioseq::storage::pg_sequence_source_provider* source_provider = nullptr;
    // Borrowed from the statement-root alignment_execution_collector.
    bioseq::pipeline_stats::persisted_gpu_operation_accumulator*
        persisted_gpu_operation_stats_sink = nullptr;
    slice_async_owner_guard* slice_async_owner = nullptr;
    bioseq::storage::sequence_payload_allocation_observer
        mmsearch_target_payload_observer{};
    bioseq::storage::sequence_payload_allocation_observer
        mmsearch_query_payload_observer{};
    MemoryContext batch_payload_ctx = nullptr;
    /* Reused virtual serializer input for rows containing expanded recipe
     * Datums.  Keeping this handle in member storage makes the PG_TRY error
     * path deterministic: a longjmp may abandon the current copy, but cannot
     * leave an untracked automatic slot or poison the next submitted row. */
    TupleTableSlot* copy_staging_slot = nullptr;
    std::array<std::vector<storage_manager_type::fetch_request>, storage_manager_type::bucket_count>
        per_bucket_target_requests{};
    std::array<std::vector<storage_manager_type::fetch_request>, storage_manager_type::bucket_count>
        per_bucket_query_requests{};
    std::vector<storage_manager_type::fetch_request> fetch_bulk_sort_scratch{};
    std::vector<bioseq::pg_types::seq*>          hydrated_target_payloads{};
    std::vector<bioseq::pg_types::seq*>          hydrated_query_payloads{};
    pg_sequence_read_session* active_wave_read_session = nullptr;
    // Member storage is intentional: PG_TRY/PG_CATCH uses setjmp/longjmp, so
    // a modified automatic optional would have an indeterminate catch value.
    std::optional<bioseq::parallelism::memory_permit> active_wave_permit{};
    /* §9.6 EXPLAIN counters. */
    std::uint64_t explain_bucket_touched_mask      = 0;
    std::uint64_t explain_storage_bulk_fetch_calls = 0;

    struct evaluated_row_operands {
        Datum target = Datum{0};
        bool target_is_null = true;
        bool has_query_override = false;
        Datum query = Datum{0};
        bool query_is_null = true;
    };

    void require_evaluated_mmsearch_index_policy_() const;

    static void require_registered_mvcc_snapshot_(Snapshot snapshot);

    [[nodiscard]] Datum retain_expanded_recipe_(Datum input);

    static void require_recipe_descriptor_(
            const pg_sequence_descriptor& descriptor,
            const std::uint8_t alphabet_code,
            const std::uint8_t storage_unit_byte_width,
            const char* operand);

    void capture_target_datum_(buffered_scan_entry& entry, Datum datum);

    void capture_query_datum_(buffered_scan_entry& entry, Datum datum);

    void retain_expanded_sequence_output_attributes_(
            buffered_scan_entry& entry,
            TupleTableSlot* slot);

public:
    explicit tuple_dispatch_buffer(const size_t estimated_tuple_count);

    /// Bind the sole statement-root owner used by every batch/node/rescan.
    void bind_persisted_gpu_operation_stats_sink(
        bioseq::pipeline_stats::persisted_gpu_operation_accumulator& sink);

    void bind_slice_async_owner(slice_async_owner_guard* owner);

    void bind_sequence_source_provider(
            bioseq::storage::pg_sequence_source_provider* provider);

    void clear_sequence_source_provider_binding() noexcept;

    /**
     * Release a provenance binding discovered from one per-row join batch.
     *
     * Leaf scan buffers are bound once by their owning slice projection and must
     * retain that binding across ordinary clear_slots() calls.  A per-row join
     * buffer is different: its owner is discovered from the returned operand
     * EOHs, and a parameterized/partitioned rescan may legitimately return a
     * different owning projector after the previous batch is fully drained.
     */
    void release_per_row_slice_async_owner_after_drain();

	/* batch join discovers its owner from the retained query relation.  The
	 * binding spans every target in one executor pass, then is released only
	 * after reset_join_runtime_for_rescan() has drained all waves. */
	void release_join_slice_async_owner_after_drain();

    /*
     * Install the per-scan aligner parameters. Called from
     * the column-scan begin callback after the GUC snapshot and any per-call
     * aligner_options() overrides have been applied. Must be called before
     * the first submit_scan.
     */
    void configure_params(const ::bioseq::simd::minimap2_parameters& params,
                          bioseq::indexing::index_read_requirement read_requirement,
                          bool read_sequence_index,
                          Oid scan_user_relid,
                          Oid scan_user_database_oid,
                          Oid scan_user_tablespace_oid,
                          Oid scan_user_main_relfilenode,
                          Relation executor_relation,
                          Snapshot executor_snapshot,
                          MemoryContext executor_query_context);

    // Parallel workers restore the leader's serialized scan snapshot only
    // after BeginCustomScan configured this buffer. Rebind the borrowed
    // pointer before the worker can dispatch any row; ownership remains with
    // scan_state.
    void replace_executor_snapshot(Snapshot executor_snapshot);

    [[nodiscard]] const ::bioseq::simd::minimap2_parameters& base_aligner_params() const { return base_params; }
    [[nodiscard]] const ::bioseq::pipeline_stats::pipeline_execution_stats& execution_stats() const { return last_stats; }
    [[nodiscard]] bool reads_sequence_index() const { return read_sequence_index_active; }
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
     * Record the mmsearch column / pk / query triple extracted from the
     * plan. Called once when each column scan begins.
     * query_varlena_bytes owns the detoasted varlena — moved in.
     *
     * (alphabet_code, storage_unit_byte_width) is the COLUMN's typmod — the
     * dispatch typmod for the whole batch. The query bytes may be
     * carried at a different (alphabet, width); the caller is
     * responsible for re-encoding if a coalesce is needed. See
     * `apply_typmod_coalesce()` below.
     */
    void configure_mmsearch(AttrNumber seq_attno,
                            std::string column_name,
                            std::vector<char> query_varlena_bytes,
                            uint8_t alphabet_code,
                            uint8_t storage_unit_byte_width,
                            AttrNumber primary_key_attno);

    /*
     * Configure the per-row-query mode. Called by
     * the column-scan begin callback when the mmsearch second argument is a
     * Var of the SAME relation — i.e. each row aligns against its
     * own value of `per_row_query_attno`. No global query varlena
     * is captured up front; submit_scan extracts the per-row bytes
     * for every passing tuple, and run_batch dispatches once per
     * entry.
     *
     * `per_row_self_alignment` is purely informational here (used
     * only by EXPLAIN to surface "self-alignment" instead of
     * "per-row alignment"). Both modes go through the same
     * dispatch path; we never short-circuit the aligner.
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

    /* Evaluated target/query expressions are submitted explicitly for each
     * passing base row.  The physical seq_attno remains source metadata and
     * identifies the raw output attribute, but is never read as an operand by
     * this mode. */
    void configure_mmsearch_per_row_evaluated(
            const AttrNumber seq_attno,
            std::string column_name,
            const std::uint8_t alphabet_code,
            const std::uint8_t storage_unit_byte_width,
            const std::uint8_t query_alphabet_code,
            const std::uint8_t query_storage_unit_byte_width,
            const AttrNumber primary_key_attno);

    /* Configure the narrow two-child bioseq batch join seam. */
    void configure_mmsearch_join(
            const Oid target_relation_oid,
            const AttrNumber target_storage_seq_attno,
            const AttrNumber target_slot_seq_resno,
            const AttrNumber target_slot_rowno_resno,
            const AttrNumber query_slot_seq_resno,
            std::string target_column_name,
            const std::uint8_t alphabet_code,
            const std::uint8_t storage_unit_byte_width,
            const std::uint8_t query_alphabet_code,
            const std::uint8_t query_storage_unit_byte_width);

    /* Pin/copy one locator-passthrough target for all query batches. */
    void begin_join_target(TupleTableSlot* target_slot);

private:
	static void prepare_join_cpu_after_highway_selection_thunk(
			void* state,
			::bioseq::simd::batch_processor::scanner_pg_context& scanner_pg_ctx,
			const ::bioseq::simd::minimap2_parameters& dispatched_params,
			const ::bioseq::simd::batch_processor::cpu_preparation_typmod_operations&
				typmod_ops);

	void prepare_join_cpu_after_highway_selection(
			::bioseq::simd::batch_processor::scanner_pg_context& scanner_pg_ctx,
			const ::bioseq::simd::minimap2_parameters& dispatched_params,
			const ::bioseq::simd::batch_processor::cpu_preparation_typmod_operations&
				typmod_ops);

public:

    void prepare_join_target_queries(std::span<const join_query_ref> all_queries);

    void set_join_query_batch(std::span<const join_query_ref> queries);

    /*
     * Re-encode the literal query under (target_alphabet,
     * target_width) so the batch_processor's static dispatch agrees
     * with the column's typmod. Caller passes the detoasted bytes
     * of the re-encoded value (typically built via the same
     * convert_from(cstr, …, pg_alloc) path used by seq_in / seq_cast).
     *
     * The ORIGINAL query_varlena is preserved (so EXPLAIN can
     * surface the literal the user wrote) and `query_hdr()`
     * returns the coalesced bytes whenever `coalesce_active` is
     * true.
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
     * Reset the primary mmsearch context. Used by:
     *   - BeginCustomScan as a defensive reset before re-installing
     *     the per-query primary (extras get added in the same Begin
     *     pass; clearing them here is correct because BeginCustomScan
     *     re-populates everything from scratch).
     *   - ReScanCustomScan ONLY when the per-rescan Param-rebind
     *     fails (Param resolved to NULL / wrong paramkind). After §9.4
     *     enabled Param-primary + Const-extras, the extras must
     *     SURVIVE a temporary primary failure: a NL where outer row
     *     #1 has a NULL Param and outer row #2 has a non-NULL one
     *     should still apply the extras for row #2. The extras are
     *     registered ONCE in BeginCustomScan and never re-added on
     *     rescan, so clearing them here would silently lose them
     *     for every later rescan — the §9 review caught this.
     *
     * `clear_extras=true` applies the complete reset semantics
     * (used by BeginCustomScan defensive init only). Default is
     * false so ReScanCustomScan and any other caller preserves the
     * extras list across rebinds.
     */
    void clear_mmsearch_context(bool clear_extras = false);

    /*
     * Append another Const-driven mmsearch query that shares the
     * primary context's column and typmod. The caller (BeginCustomScan)
     * is responsible for validating the column / typmod match before
     * calling — we only own ownership of the bytes here.
     *
     * The accepted shape is exactly one same-column same-typmod
     * mmsearch beyond the first. Any extra query with a different
     * column or typmod must be rejected by the caller; if added here
     * it would silently get dispatched against the first context's
     * (seq, alphabet, width) and produce nonsense matches.
     */
    void add_extra_const_query(std::vector<char> query_varlena_bytes);

    [[nodiscard]] size_t extra_query_count() const;

    /*
     * Writing-stage hook. Fired from Inner_CustomExecScan after ExecQual
     * passes, using econtext->ecxt_scantuple which still holds every
     * attribute of the base relation. Extracts the bioseq.seq column
     * named by configure_mmsearch (detoasting its varlena), the PK
     * column value, and copies the slot into a MinimalTuple-backed
     * clone so the data outlives the next ExecScanFetch call.
     */
    [[gnu::always_inline]] inline void submit_scan(TupleTableSlot* scan_slot) {
        submit_scan_impl_(scan_slot, nullptr);
    }

    void submit_scan_evaluated(
            TupleTableSlot* scan_slot,
            const Datum target,
            const bool target_is_null,
            const bool has_query_override,
            const Datum query,
            const bool query_is_null) {
        require_evaluated_mmsearch_index_policy_();
        if (has_query_override != per_row_mode()) {
            ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("evaluated bioseq mmsearch query submission disagrees with its configuration"),
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
        submit_scan_impl_(scan_slot, std::addressof(evaluated));
    }

private:
    [[gnu::always_inline]] inline void submit_scan_impl_(
            TupleTableSlot* scan_slot,
            const evaluated_row_operands* evaluated) {
        if (!mmsearch_state.active) {
            bioseq_log<logging_level::debug>("[bioseq] pg: submit_scan called while mmsearch state inactive; skipping");
            return;
        }
        if (scan_slot == nullptr) {
            bioseq_log<logging_level::debug>("[bioseq] pg: submit_scan received null scan_slot; skipping");
            return;
        }
        if (buffered_entries.size() >= dispatch_batch_capacity) {
            ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("bioseq dispatch buffer capacity invariant was violated"),
                 errdetail("buffered=%zu capacity=%zu",
                    buffered_entries.size(), dispatch_batch_capacity)));
        }

        buffered_scan_entry entry{};

        /*
         * Capture the scan slot's tts_tid (set by the bioseq columnar
         * access method in the column-scan iterator) and invert it back to a
         * row number so downstream consumers can join against the
         * sidecar tables (keyed on (user_relid, sequence_rowno)). Uses
         * the canonical tid_to_row_number helper from access_method.cppm
         * so the encoding stays in lockstep with row_number_to_tid.
         */
        if (ItemPointerIsValid(&scan_slot->tts_tid)) {
            entry.sequence_rowno = static_cast<int64_t>(
                tid_to_row_number(scan_slot->tts_tid));
        }

        /* ExprState NULL is authoritative for an evaluated operand.  Do not
         * consult the physical seq attribute: it may be non-NULL while the
         * expression is NULL, or vice versa. */
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
                /* Flat/TOAST values retain the established compact fast path;
                 * locators retain their decoded source identity; expanded
                 * slice projection values retain only a stable piece-tree wrapper.
                 * In particular, never send an expanded Datum through
                 * PG_DETOAST_DATUM here: that would flatten the complete
                 * transformed value before algorithm admission. */
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
         * Per-row-query mode: extract the value of the OTHER bioseq.seq
         * column referenced by the mmsearch second arg. For
         * self-alignment (`per_row_self_alignment` true and
         * per_row_query_attno == seq_attno) this is literally the same
         * column we just buffered — we still re-extract so the path is
         * uniform between sibling-column and self-column shapes.
         * Per-row-query bytes retain the query column's declared typmod in the
         * copied output slot. The private dispatch copy is rewidthed once to
         * the target shape when both operands bind the same alphabet.
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
                /* Evaluate and retain target/query independently.  In
                 * particular, a self-alignment may contain two different
                 * slices of the same physical base column and must not reuse
                 * the target recipe merely because the raw attnos match. */
                capture_query_datum_(entry, evaluated->query);
            }
        } else if (mmsearch_state.per_row_query_attno
                != InvalidAttrNumber) {
            bool query_null = true;
            Datum query_datum = slot_getattr(scan_slot,
                mmsearch_state.per_row_query_attno, &query_null);
            if (query_null) {
                entry.per_row_query_is_null = true;
            } else if (mmsearch_state.per_row_query_attno
                           == mmsearch_state.seq_attno
                       && entry.has_seq_recipe()) {
                /* A self-alignment needs one retained wrapper, not two
                 * independently retained source graphs. */
                require_recipe_descriptor_(
                    entry.seq_recipe_descriptor,
                    mmsearch_state.query_alphabet_code,
                    mmsearch_state.query_storage_unit_byte_width,
                    "query");
                entry.per_row_query_recipe_datum = entry.seq_recipe_datum;
                entry.per_row_query_recipe_descriptor =
                    entry.seq_recipe_descriptor;
                entry.per_row_query_is_null = false;
            } else {
                capture_query_datum_(entry, query_datum);
            }
        }

        /*
         * Force materialization of every attribute before cloning. The
         * columnar scan only populates tts_values[] for attributes in
         * attributes_needed; anything outside that set carries a stale
         * Datum. ExecCopySlotMinimalTuple serialises tts_values[] to a
         * MinimalTuple, and slot_getallattrs() guarantees every entry
         * is valid before serialisation.
         */
        slot_getallattrs(scan_slot);
        retain_expanded_sequence_output_attributes_(entry, scan_slot);

        /* ExecCopySlotMinimalTuple materializes expanded attributes.  Mask
         * only the retained recipe columns while serializing the rest of the
         * tuple, then reinstall the batch-owned wrappers in the copied slot.
         * The source slot is never mutated and can be reused by its child. */
        TupleTableSlot* copy_source = scan_slot;
        if (batch_payload_ctx == nullptr) {
            ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("bioseq dispatch tuple arena is not initialized")));
        }
        MemoryContext previous_copy_context = MemoryContextSwitchTo(
            batch_payload_ctx);
        if (!entry.expanded_sequence_attributes.empty()) {
            if (copy_staging_slot == nullptr) {
                copy_staging_slot = MakeSingleTupleTableSlot(
                    scan_slot->tts_tupleDescriptor, &TTSOpsVirtual);
            } else if (copy_staging_slot->tts_tupleDescriptor
                    != scan_slot->tts_tupleDescriptor) {
                ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("bioseq dispatch staging slot descriptor changed")));
            }
            ExecClearTuple(copy_staging_slot);
            const int natts = scan_slot->tts_tupleDescriptor->natts;
            std::memcpy(
                copy_staging_slot->tts_values, scan_slot->tts_values,
                static_cast<std::size_t>(natts) * sizeof(Datum));
            std::memcpy(
                copy_staging_slot->tts_isnull, scan_slot->tts_isnull,
                static_cast<std::size_t>(natts) * sizeof(bool));
            const auto mask_recipe = [this, natts](AttrNumber attno) {
                const int index = static_cast<int>(attno) - 1;
                if (index >= 0 && index < natts) {
                    copy_staging_slot->tts_values[index] = Datum{0};
                    copy_staging_slot->tts_isnull[index] = true;
                }
            };
            for (const auto& expanded :
                 entry.expanded_sequence_attributes) {
                mask_recipe(expanded.attno);
            }
            copy_staging_slot->tts_nvalid = natts;
            ExecStoreVirtualTuple(copy_staging_slot);
            copy_source = copy_staging_slot;
        }

        entry.scan_slot_copy = MakeSingleTupleTableSlot(
            scan_slot->tts_tupleDescriptor, &TTSOpsMinimalTuple);
        ExecStoreMinimalTuple(ExecCopySlotMinimalTuple(copy_source),
                              entry.scan_slot_copy, /*shouldFree=*/true);
        MemoryContextSwitchTo(previous_copy_context);

        /*
         * Eagerly materialize all attributes of the copied slot into
         * tts_values[]/tts_isnull[]. Without this, a downstream
         * ExecBuildProjectionInfo + ExecProject sees tts_nvalid=0 and
         * still works via the deopt getsomeattrs callback — but during
         * debugging we observed cases where the path built against the
         * copied slot's tupdesc fell back to zero-valued reads. Forcing
         * materialization now removes that ambiguity for zero cost.
         */
        slot_getallattrs(entry.scan_slot_copy);
        bioseq::postgres::support::pg_slot_datums_view copied_values{
            entry.scan_slot_copy};
        for (const auto& expanded :
             entry.expanded_sequence_attributes) {
            copied_values.set_raw(
                expanded.attno,
                bioseq::postgres::support::pg_datum::from_raw(
                    expanded.datum));
        }

        /*
         * No per-row blob read occurs here. The stored
         * index (if any) is ASSEMBLED at batch time from its chunked storage,
         * keyed by entry.sequence_rowno (the columnar rowno == index
         * query_key, already computed above), so submit_scan only needs the
         * rowno (carried into scanner_pg_ctx.target_rownos[]).
         */
        const std::uint64_t entry_seq_bp_size = entry.seq_bp_size();
        bioseq_log<logging_level::debug>("[bioseq] pg: submit_scan pk={} rowno={} seq_bp_size={} (null={})",
                entry.primary_key_is_null ? -1 : entry.primary_key,
                entry.sequence_rowno,
                entry_seq_bp_size,
                entry.seq_is_null);

        buffered_entries.push_back(std::move(entry));
    }

public:
    /* True exactly when the current physical buffer must be dispatched. */
    [[nodiscard]] bool ready_to_dispatch() const noexcept {
        return buffered_entries.size() == dispatch_batch_capacity;
    }

    /*
     * Per-bucket bulk hydration of one sequence column. Groups the
     * buffered entries that captured a locator for `column_attno` by
     * bucket_index and issues one storage_manager_type::fetch_payload_bulk per non-empty bucket
     * (each opens that bucket's shards once and scans them row-number-sorted).
     * The hydrated payloads (palloc'd in batch_payload_ctx) are recorded on
     * the entry so seq_hdr()/per_row_query_hdr() return them, and written back
     * into the buffered output slot so a target list projecting the raw seq
     * column resolves the Var to the materialised payload, not the stub.
     */
    void hydrate_one_column(
            std::span<const size_t> rows,
            bioseq::storage::seq_caches* cache,
            std::int16_t column_attno,
            std::array<std::vector<storage_manager_type::fetch_request>, storage_manager_type::bucket_count>& per_bucket,
            std::vector<bioseq::pg_types::seq*>& out_payloads,
            bool is_query);

    /* Integration-only in-memory baselines. Production scanner dispatch must
     * not call this routine: it would stitch every locator into a flat varlena
     * before the source-domain facade had a chance to stream it. */
    void hydrate_locators_by_bucket(std::span<const size_t> rows);

    [[nodiscard]] bool run_join_query_batch_and_build_cursors();

    void finish_join_target();

    /// Zero-PostgreSQL failure abandonment for the CustomScan executor seam.
    /// The transaction/query ResourceOwners reclaim tuple slots, detoast
    /// copies, readers and MemoryContexts. This method only severs those PG
    /// handles and destroys ordinary C++ containers/permits after a bridged
    /// ERROR, so cleanup cannot replace the original ErrorData.
    void abandon_join_runtime_after_pg_error_noexcept() noexcept;

    void reset_join_runtime_for_rescan();

    /*
     * Batch-stage transition. Called when the access method returns
     * TupIsNull. Invokes batch_processor::process_sequences on the
     * buffered target varlenas plus the stashed query varlena; stashes
     * the returned match-result map; builds output_cursors in insertion
     * order (one per match result per row, skipping rows whose PK is
     * absent from the map). The query/target type dispatch happens
     * inside process_sequences via bioseq::pg_types::seq::dispatch_instance.
     */
    [[nodiscard]] bool run_batch_and_build_cursors();

    void run_batch_and_build_cursors_impl();

    /*
     * Reading-stage iterator. nullopt means only genuine cursor exhaustion.
     * A cursor whose entry or match no longer exists is internal corruption,
     * not EOF, and therefore raises ERROR fail closed.
     */
    [[nodiscard]] std::optional<next_output> read_next_output();

    /*
     * True once run_batch_and_build_cursors has been called for the
     * current scan. Inner_CustomExecScan uses this to switch from the
     * writing-stage access-method loop to the reading-stage drain loop.
     */
    [[nodiscard]] constexpr bool has_finished_writing() const {
        return output_position >= 0;
    }

    /*
     * True when BeginCustomScan found a WHERE mmsearch(...) qual and
     * installed its (seq_attno, query_varlena, typmod) context. When
     * false the custom scan must behave like a pass-through scan:
     * fetch rows, project them directly via the (variable injection mutator-
     * rewritten) target list, and return them without buffering. The
     * buffering + batch pipeline is only meaningful when there is a
     * WHERE mmsearch(...) to drive it. See SPEC 2 §6.8 for the
     * lifecycle gap this accessor closes.
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
     * Reset writing/reading state so a new scan can start. Frees the
     * MinimalTuple slots allocated in submit_scan; leaves
     * mmsearch state untouched so callers can reuse configuration.
     */
    [[gnu::always_inline]] inline void clear_slots() {
        bioseq_log<logging_level::debug>("[bioseq] pg: tuple_dispatch_buffer::clear_slots() entered; freeing {} slots, {} outputs",
                buffered_entries.size(), output_cursors.size());
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
        ::abort_pg_sequence_read_session_noexcept(
            std::exchange(active_wave_read_session, nullptr));
        active_wave_permit.reset();
        if (copy_staging_slot != nullptr) {
            ExecDropSingleTupleTableSlot(copy_staging_slot);
            copy_staging_slot = nullptr;
        }
        /*
         * The bulk-fetched payloads (and the per-row query
         * payloads) live in batch_payload_ctx and are valid until exactly
         * here: clear_slots() runs after the batch's output has been drained
         * and before the next buffering cycle (e.g. a Nested Loop rescan), so
         * resetting now reclaims the previous batch's payload arena without
         * dangling any hydrated_seq_hdr the just-cleared entries referenced.
         */
        if (batch_payload_ctx != nullptr) {
            MemoryContextReset(batch_payload_ctx);
        }
    }

    /*
     * Total-reset shorthand: drop every buffered slot, the
     * mmsearch primary AND every registered Const-extra. Equivalent
     * to "as if the buffer had never been configured". This is
     * stricter than `clear_mmsearch_context()` (which preserves
     * extras across rescans by default — see §9.4 / §9-review fix).
     * Kept for symmetry with the pre-§9 API; not used by any live
     * caller today, but the explicit `true` makes the contract
     * loud rather than relying on the default's semantics.
     */
    [[gnu::always_inline]] inline void clear() {
        clear_slots();
        clear_mmsearch_context(/*clear_extras=*/true);
    }
};

static_assert(noexcept(
    std::declval<tuple_dispatch_buffer&>()
        .abandon_join_runtime_after_pg_error_noexcept()));

} // namespace bioseq::scheduler
