/*-------------------------------------------------------------------------
 *
 * scheduler/dispatch_buffer.cppm
 *
 * TupleDispatchBuffer accumulates TupleTableSlot copies produced during
 * a custom-scan execution phase so the scan node can later replay them in
 * order. The scan runs in two phases per query:
 *
 *   Writing phase (driven by submit_scan): for every source tuple that
 *   passed the WHERE qual, the scan copies the slot into the buffer and
 *   records the row's primary key and a detoasted copy of its bioseq.seq
 *   varlena. Projection does NOT happen here; `ecxt_scantuple` still
 *   references the original scan slot which is about to be overwritten
 *   by the next call.
 *
 *   Batch phase (run_batch_and_build_cursors): after the access method
 *   signals EOF, the buffer feeds its target varlenas (plus the mmsearch
 *   query varlena captured at BeginCustomScan) into
 *   BatchProcessor::process_sequences. That function dispatches on the
 *   column's (alphabet_code, storage_unit_byte_width) typmod to instantiate the right
 *   compact_sequence_range<Dst> specialisation, then invokes
 *   minimap_port(query, target) for every row and stores the returned
 *   MatchResults keyed by primary key. Rows whose PK is missing from the
 *   returned map are filtered; this is how WHERE mmsearch(...) evaluates
 *   to FALSE for those rows.
 *
 *   Reading phase (read_next_output): the custom scan iterates over
 *   output cursors, pointing `ecxt_scantuple` at the buffered scan slot
 *   and projecting via ExecProject. The placeholder Const* nodes
 *   injected by VarInjectorMutator are updated from the MatchResult
 *   before projection fires.
 *
 *-------------------------------------------------------------------------
 */
module;

#include <hwy/highway.h>  // only for HWY_NAMESPACE when naming active-target scheduler types
#include "common.h"
extern "C" {
#include "utils/snapmgr.h"
}

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

// DOC0263 §11.3.2 / DOC0264 — the chunked-scanner dispatch path is always
// compiled (Phase C made the fragmented pipeline tree a permanent fixture).
// The `bioseq.test_force_chunked_scanner` GUC routes row sequences through a
// synthetic chunked scanner end-to-end (DOC0263 parity tests). The
// `bioseq.test_force_inmemory_scanner` GUC forces the materialise-then-
// in_memory baseline. Without either GUC the production path runs through
// `with_lazy_postgres<S>` (DOC0264 §9.1).
export module bioseq.scheduler.dispatch_buffer;

import bioseq.common.mlogging;
import bioseq.pg_utils;
import bioseq.config;                               // DOC0322 §9.3 — bioseq_index_async_persist, bioseq_recompute_async_persist GUCs
import bioseq.indexing.type;                 // DOC0279 — DIRECTORY_SUB_KEY
import bioseq.storage.indexing_pg.handles;      // DOC0279 §8.7/§12.3 — lightweight storage handles + active index
import bioseq.scheduler.dispatch_processor;   // fixed-shape BatchProcessor, heavy pipeline in its own module
import bioseq.sequences;
import bioseq.sequences.minimap_port;
import bioseq.sequences.cigar;
import bioseq.sequences.pipe;
import bioseq.types.pg_compact_sequence;
import bioseq.instr;
import bioseq.storage.manager;                  // DOC0260 §9
import bioseq.storage.scanner;          // DOC0263 §11.3.2 — chunked test scanner — seq_locator, fetch_payload_bulk, seq_caches
import bioseq.parallelism.memory_admission;
import bioseq.scheduler.supervisor_pg;
import bioseq.column.tam;               // DOC0322 §7.4 — tid_to_row_number (leaf; replaces the former extern "C++" reach into the table AM)

export namespace bioseq::scheduler {

constexpr size_t kDispatchBatchCapacity = 8192;

[[nodiscard]] constexpr std::uint64_t extra_broadcast_query_ordinal(
        const bool per_row_query_mode,
        const std::size_t batch_size,
        const std::size_t extra_query_index) noexcept {
    return per_row_query_mode
        ? static_cast<std::uint64_t>(batch_size)
            + static_cast<std::uint64_t>(extra_query_index)
        : static_cast<std::uint64_t>(extra_query_index) + 1;
}
static_assert(extra_broadcast_query_ordinal(false, 2, 0) == 1);
static_assert(extra_broadcast_query_ordinal(true, 2, 0) == 2);
static_assert(
    std::uint64_t{1} != extra_broadcast_query_ordinal(true, 2, 0),
    "per-row primary row 1 and the first extra broadcast query must differ");

enum class CursorBoundary : std::uint8_t {
    value,
    exact_eof,
    corrupt,
};

[[nodiscard]] constexpr CursorBoundary classify_output_cursor(
        const std::int64_t position,
        const std::size_t size) noexcept {
    if (position < 0) return CursorBoundary::corrupt;
    const auto unsigned_position = static_cast<std::uint64_t>(position);
    if (unsigned_position == size) return CursorBoundary::exact_eof;
    return unsigned_position < size
        ? CursorBoundary::value
        : CursorBoundary::corrupt;
}

[[nodiscard]] constexpr CursorBoundary classify_output_cursor(
        const std::size_t position,
        const std::size_t size) noexcept {
    if (position == size) return CursorBoundary::exact_eof;
    return position < size
        ? CursorBoundary::value
        : CursorBoundary::corrupt;
}

static_assert(classify_output_cursor(std::int64_t{0}, 1)
    == CursorBoundary::value);
static_assert(classify_output_cursor(std::int64_t{1}, 1)
    == CursorBoundary::exact_eof);
static_assert(classify_output_cursor(std::int64_t{-1}, 1)
    == CursorBoundary::corrupt);
static_assert(classify_output_cursor(std::int64_t{2}, 1)
    == CursorBoundary::corrupt);
static_assert(classify_output_cursor(std::size_t{0}, 0)
    == CursorBoundary::exact_eof);
static_assert(classify_output_cursor(std::size_t{2}, 1)
    == CursorBoundary::corrupt);

[[nodiscard]] constexpr bool shared_join_raw_target_shape_valid(
		const ::bioseq::pipeline_stats::ExecutedBackend backend,
		const std::uint64_t query_count,
		const std::uint64_t attempted_jobs,
		const std::uint64_t raw_target_rows) noexcept {
	if (query_count == 0 || attempted_jobs != query_count) {
		return false;
	}
	std::uint64_t expected = 0;
	switch (backend) {
		case ::bioseq::pipeline_stats::ExecutedBackend::scalar_cpu:
		case ::bioseq::pipeline_stats::ExecutedBackend::simd_cpu:
			expected = query_count;
			break;
		case ::bioseq::pipeline_stats::ExecutedBackend::vulkan_gpu:
			expected = 1;
			break;
		case ::bioseq::pipeline_stats::ExecutedBackend::none:
		case ::bioseq::pipeline_stats::ExecutedBackend::mixed_invalid:
			return false;
	}
	return raw_target_rows == expected;
}
static_assert(shared_join_raw_target_shape_valid(
	::bioseq::pipeline_stats::ExecutedBackend::simd_cpu,
	16, 16, 16));
static_assert(shared_join_raw_target_shape_valid(
	::bioseq::pipeline_stats::ExecutedBackend::vulkan_gpu,
	16, 16, 1));
/* Later waves must still carry native raw target/source evidence.  Only the
 * BatchJoin owner suppresses their normalized publication. */
static_assert(shared_join_raw_target_shape_valid(
	::bioseq::pipeline_stats::ExecutedBackend::simd_cpu,
	8192, 8192, 8192));
static_assert(!shared_join_raw_target_shape_valid(
	::bioseq::pipeline_stats::ExecutedBackend::simd_cpu,
	1, 1, 0));
static_assert(!shared_join_raw_target_shape_valid(
	::bioseq::pipeline_stats::ExecutedBackend::mixed_invalid,
	16, 16, 16));
static_assert(!shared_join_raw_target_shape_valid(
	::bioseq::pipeline_stats::ExecutedBackend::simd_cpu,
	16, 15, 16));
static_assert(!shared_join_raw_target_shape_valid(
	::bioseq::pipeline_stats::ExecutedBackend::simd_cpu,
	16, 16, 15));

[[nodiscard]] constexpr bool shared_join_raw_source_shape_valid(
		const ::bioseq::pipeline_stats::TargetIndexSource requested,
		const ::bioseq::pipeline_stats::TargetIndexSource actual,
		const std::uint64_t raw_target_rows,
		const std::uint64_t persisted_rows,
		const std::uint64_t recomputed_rows,
		const std::uint64_t missing_rows) noexcept {
	if (raw_target_rows == 0
		|| requested == ::bioseq::pipeline_stats::TargetIndexSource::none
		|| requested
			== ::bioseq::pipeline_stats::TargetIndexSource::mixed_invalid
		|| actual == ::bioseq::pipeline_stats::TargetIndexSource::none
		|| actual
			== ::bioseq::pipeline_stats::TargetIndexSource::mixed_invalid) {
		return false;
	}
	if (actual == ::bioseq::pipeline_stats::TargetIndexSource::persisted) {
		/* A recompute-only request must never discover/use persisted payload. */
		return requested
				== ::bioseq::pipeline_stats::TargetIndexSource::persisted
			&& persisted_rows == raw_target_rows
			&& recomputed_rows == 0
			&& missing_rows == 0;
	}
	/* The only remaining concrete actual source is recomputed. */
	if (persisted_rows != 0 || recomputed_rows != raw_target_rows) {
		return false;
	}
	if (requested
		== ::bioseq::pipeline_stats::TargetIndexSource::recomputed) {
		return missing_rows == 0;
	}
	/* An allow-mode persisted request may truthfully miss and recompute, but
	 * the producer must report one missing outcome for every raw target. */
	return requested
			== ::bioseq::pipeline_stats::TargetIndexSource::persisted
		&& missing_rows == raw_target_rows;
}
static_assert(shared_join_raw_source_shape_valid(
	::bioseq::pipeline_stats::TargetIndexSource::recomputed,
	::bioseq::pipeline_stats::TargetIndexSource::recomputed,
	16, 0, 16, 0));
static_assert(shared_join_raw_source_shape_valid(
	::bioseq::pipeline_stats::TargetIndexSource::persisted,
	::bioseq::pipeline_stats::TargetIndexSource::persisted,
	16, 16, 0, 0));
static_assert(shared_join_raw_source_shape_valid(
	::bioseq::pipeline_stats::TargetIndexSource::persisted,
	::bioseq::pipeline_stats::TargetIndexSource::recomputed,
	16, 0, 16, 16));
static_assert(!shared_join_raw_source_shape_valid(
	::bioseq::pipeline_stats::TargetIndexSource::persisted,
	::bioseq::pipeline_stats::TargetIndexSource::recomputed,
	16, 0, 16, 0));
static_assert(!shared_join_raw_source_shape_valid(
	::bioseq::pipeline_stats::TargetIndexSource::recomputed,
	::bioseq::pipeline_stats::TargetIndexSource::persisted,
	16, 16, 0, 0));
static_assert(!shared_join_raw_source_shape_valid(
	::bioseq::pipeline_stats::TargetIndexSource::mixed_invalid,
	::bioseq::pipeline_stats::TargetIndexSource::recomputed,
	16, 0, 16, 0));
static_assert(!shared_join_raw_source_shape_valid(
	::bioseq::pipeline_stats::TargetIndexSource::persisted,
	::bioseq::pipeline_stats::TargetIndexSource::persisted,
	16, 15, 1, 0));

/*
 * The typed processor is a generic row-pair consumer: some target/index
 * outcome paths still report one operational observation per pair.  A
 * BioseqBatchJoin wave, by contrast, owns exactly one physical target and
 * repeats only its query side.  Collapse that generic evidence at the owner
 * boundary after first proving that it is internally consistent.  This is
 * deliberately not a permissive clamp: mixed target sources, missing source
 * outcomes, or counts outside the physical wave shape fail closed.
 */
inline void normalize_shared_join_target_outcomes(
		::bioseq::pipeline_stats::PipelineExecutionStats& stats,
        const bool record_physical_target,
        const std::size_t query_count) {
	auto& operational = stats.operational;
    const std::uint64_t target_rows = operational.target_rows;
    const std::uint64_t persisted_rows = operational.persisted_index_rows;
    const std::uint64_t recomputed_rows =
        operational.recomputed_target_index_rows;
    const std::uint64_t missing_rows =
        operational.missing_persisted_index_rows;
    const std::uint64_t query_count_u64 =
        static_cast<std::uint64_t>(query_count);
	std::uint64_t expected_raw_target_rows = 0;
	switch (stats.requested_backend) {
		case ::bioseq::pipeline_stats::ExecutedBackend::scalar_cpu:
		case ::bioseq::pipeline_stats::ExecutedBackend::simd_cpu:
			expected_raw_target_rows = query_count_u64;
			break;
		case ::bioseq::pipeline_stats::ExecutedBackend::vulkan_gpu:
			expected_raw_target_rows = 1;
			break;
		case ::bioseq::pipeline_stats::ExecutedBackend::none:
		case ::bioseq::pipeline_stats::ExecutedBackend::mixed_invalid:
			ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("BioseqBatchJoin target outcomes have no attested requested backend"),
				 errdetail("requested_backend=%.*s query_count=%zu",
					static_cast<int>(
						::bioseq::pipeline_stats::executed_backend_name(
							stats.requested_backend).size()),
					::bioseq::pipeline_stats::executed_backend_name(
						stats.requested_backend).data(),
					query_count)));
	}
    const bool source_mixed = persisted_rows != 0 && recomputed_rows != 0;
    const bool source_count_invalid =
        persisted_rows > query_count_u64
        || recomputed_rows > query_count_u64
        || persisted_rows + recomputed_rows != target_rows;
	const bool raw_target_shape_invalid =
		!shared_join_raw_target_shape_valid(
			stats.requested_backend,
			query_count_u64,
			stats.attempted_alignment_jobs,
			target_rows);
	const bool raw_source_shape_invalid =
		!shared_join_raw_source_shape_valid(
			stats.requested_target_index_source,
			stats.actual_target_index_source,
			target_rows,
			persisted_rows,
			recomputed_rows,
			missing_rows);
    const bool shape_invalid = query_count == 0
		|| raw_target_shape_invalid
		|| raw_source_shape_invalid
        || missing_rows > recomputed_rows
		|| (record_physical_target && target_rows == 0);
    if (source_mixed || source_count_invalid || shape_invalid) {
        ereport(ERROR,
            (errcode(ERRCODE_DATA_CORRUPTED),
             errmsg("BioseqBatchJoin target outcome accounting is inconsistent"),
             errdetail(
				"record_physical_target=%s query_count=%zu attempted_jobs=%llu "
				"requested_backend=%.*s expected_raw_target_rows=%llu "
				"requested_source=%.*s actual_source=%.*s "
				"target_rows=%llu persisted_rows=%llu recomputed_rows=%llu "
				"missing_rows=%llu",
                record_physical_target ? "true" : "false",
                query_count,
				static_cast<unsigned long long>(
					stats.attempted_alignment_jobs),
				static_cast<int>(
					::bioseq::pipeline_stats::executed_backend_name(
						stats.requested_backend).size()),
				::bioseq::pipeline_stats::executed_backend_name(
					stats.requested_backend).data(),
				static_cast<unsigned long long>(expected_raw_target_rows),
				static_cast<int>(
					::bioseq::pipeline_stats::target_index_source_name(
						stats.requested_target_index_source).size()),
				::bioseq::pipeline_stats::target_index_source_name(
					stats.requested_target_index_source).data(),
				static_cast<int>(
					::bioseq::pipeline_stats::target_index_source_name(
						stats.actual_target_index_source).size()),
				::bioseq::pipeline_stats::target_index_source_name(
					stats.actual_target_index_source).data(),
                static_cast<unsigned long long>(target_rows),
                static_cast<unsigned long long>(persisted_rows),
                static_cast<unsigned long long>(recomputed_rows),
                static_cast<unsigned long long>(missing_rows))));
    }

    operational.target_rows = record_physical_target ? 1 : 0;
    operational.persisted_index_rows =
        record_physical_target && persisted_rows != 0 ? 1 : 0;
    operational.recomputed_target_index_rows =
        record_physical_target && recomputed_rows != 0 ? 1 : 0;
    operational.missing_persisted_index_rows =
        record_physical_target && missing_rows != 0 ? 1 : 0;
}

// Fixed upper bound on one physical dispatch. The custom scan rolls over at
// this boundary, drains every result, clears the completed physical batch and
// then resumes the same source scan. No source tuple may be truncated.
class TupleDispatchBuffer {
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
    struct MmsearchContext {
        AttrNumber seq_attno           = InvalidAttrNumber;
        AttrNumber primary_key_attno   = InvalidAttrNumber;
        std::string column_name;
        std::vector<char> query_varlena;  // full detoasted varlena bytes, 16-byte aligned by std::allocator
        uint8_t alphabet_code          = 0;
        uint8_t storage_unit_byte_width             = 0;
        bool active                    = false;

        /*
         * Per-row-query attno. Set when the mmsearch second argument is a
         * Var of the SAME relation (e.g. `mmsearch(t1.seq, t1.seq)` for
         * self-alignment, or `mmsearch(t1.seq, t1.seq2)` for sibling-
         * column alignment). The planner does not parameterize same-rel
         * Vars (no Param injection happens) so the existing Const/Param
         * paths reject the qual; instead we extract the value of this
         * column at submit_scan time and store it per-row in
         * BufferedScanEntry::per_row_query_varlena. Run-batch then
         * dispatches process_sequences once per entry against that
         * row's own query — which is the only correct semantics for
         * per-row mmsearch (broadcasting one row's query to other rows
         * is exactly the bug §9 of REPORT_POSTGRES_QUERY_BUGFIXING_1.md
         * documents).
         *
         * `per_row_self_alignment` is the special case where the
         * arg1 Var has the SAME varattno as the arg0 Var (same column).
         * mmsearch(seq, seq) is then equivalent to "every row aligns
         * against itself" — guaranteed full-length match. We still
         * dispatch through the aligner so the produced MatchResult is
         * the byte-for-byte alignment that the user can inspect via
         * bioseq.cigar / target_start / etc.
         */
        AttrNumber per_row_query_attno      = InvalidAttrNumber;
        bool       per_row_self_alignment   = false;

        /*
         * Typmod coalesce: when the column and the query have the same
         * alphabet but different widths (e.g. column is dna4/32 and
         * query is dna4/64), the bytes in the query header carry the
         * larger width. The bioseq pipeline templates on
         * (alphabet_code, storage_unit_byte_width) and dispatches via
         * pgseq_t::dispatch_instance, so the query and target MUST
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
        uint8_t    raw_query_alphabet_code  = 0;
        uint8_t    raw_query_storage_unit_byte_width     = 0;

        /*
         * Cross-alphabet recompute: when the column and the query
         * differ in alphabet (e.g. column is dna4/32, query is
         * dna5/32), the sidecar minimizers — sketched at INSERT time
         * under the COLUMN's alphabet — are hash-incompatible with the
         * query. We can still align (dispatch on the column's
         * alphabet, after coalescing the query bytes), but the stored
         * index must be ignored so we recompute minimizers per row.
         * `recompute_due_to_alphabet_mismatch` is set at
         * configure_mmsearch time and consumed by run_batch to disable
         * stored-index assembly (read_index_active). EXPLAIN reads the
         * same flag to print the "Bioseq Recomputed" line with an
         * explanatory "Performance reduced" note.
         */
        bool       recompute_due_to_alphabet_mismatch = false;

        [[nodiscard]] const pgseq_t* query_hdr() const {
            const std::vector<char>& bytes =
                coalesce_active ? coalesced_query_varlena : query_varlena;
            if (!active || bytes.empty()) {
                return nullptr;
            }
            return reinterpret_cast<const pgseq_t*>(bytes.data());
        }
    };

    /*
     * One buffered scan tuple. The slot copy holds the full base-relation
     * tuple so projection can resolve any Var reference at read time.
     * primary_key is the detoasted INT4 value of the PK column; it is
     * used as the map key by the batch processor. seq_varlena is a
     * detoasted copy of the mmsearch target column's bioseq.seq value;
     * it is passed through BatchProcessor::process_sequences as the
     * per-row target sequence. Copying rather than pointing into the
     * slot is mandatory: tts_values[] is reused across scan calls and
     * the TOAST-detoasted buffer lives in CurrentMemoryContext which
     * may reset between rows.
     */
    struct BufferedScanEntry {
        TupleTableSlot* scan_slot_copy      = nullptr;
        int32_t         primary_key         = -1;
        bool            primary_key_is_null = true;
        int64_t         sequence_rowno      = -1;   /* derived from scan_slot->tts_tid; -1 when the TID was invalid */
        std::vector<char> seq_varlena;
        bool            seq_is_null         = true;

        /*
         * Per-row mmsearch query, populated only when MmsearchContext's
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

        /*
         * DOC0260 §9.1 — Phase A deferred-hydration path. Under
         * defer_seq_hydration the custom scan returns 40-byte locator stubs
         * instead of materialised payloads, so submit_scan records the
         * decoded locator here (leaving seq_varlena empty) and
         * run_batch_and_build_cursors bulk-fetches the real payload grouped
         * per bucket into batch_payload_ctx, writing the result into
         * hydrated_seq_hdr. seq_hdr() prefers the hydrated pointer, so every
         * downstream consumer (target_hdrs, BatchProcessor, output
         * projection) is unchanged. The seq_varlena path is still the
         * fallback for a non-stub value (a small inline payload).
         */
        std::optional<bioseq::storage::seq_locator> seq_loc;
        std::optional<bioseq::storage::seq_locator> per_row_query_loc;
        const pgseq_t* hydrated_seq_hdr           = nullptr;
        const pgseq_t* hydrated_per_row_query_hdr = nullptr;

        [[nodiscard]] const pgseq_t* seq_hdr() const {
            if (hydrated_seq_hdr != nullptr) {
                return hydrated_seq_hdr;
            }
            if (seq_is_null || seq_varlena.empty()) {
                return nullptr;
            }
            return reinterpret_cast<const pgseq_t*>(seq_varlena.data());
        }

        [[nodiscard]] const pgseq_t* per_row_query_hdr() const {
            if (hydrated_per_row_query_hdr != nullptr) {
                return hydrated_per_row_query_hdr;
            }
            if (per_row_query_is_null || per_row_query_varlena.empty()) {
                return nullptr;
            }
            return reinterpret_cast<const pgseq_t*>(
                per_row_query_varlena.data());
        }
    };

    /*
     * A cursor into (buffered_entries, match_map). output_cursors is
     * built in insertion order so EXPLAIN output mirrors INSERT order.
     */
    struct OutputCursor {
        size_t scan_entry_index = 0;
        size_t match_index      = 0;
    };

    struct NextOutput {
        BufferedScanEntry* entry;
        std::uint64_t query_ordinal;
        const ::bioseq::simd::HWY_NAMESPACE::MatchResult* match;
    };

    struct JoinQueryRef {
        std::uint64_t ordinal = 0;
        TupleTableSlot* query_slot = nullptr;
    };

    struct JoinOutputCursor {
        std::size_t query_index = 0;
        std::size_t match_index = 0;
    };

private:
    ::bioseq::simd::HWY_NAMESPACE::BatchProcessor processor;
    MmsearchContext mmsearch_context;

    /*
     * Per-scan base Minimap2Params — captured by BioseqColumnScan_BeginCustomScan
     * from the plan (GUC snapshot + per-call aligner_options() overrides) and
     * pushed into BatchProcessor::process_sequences once per batch. Every
     * row inherits these values with size-heuristic tuning applied if the
     * user left the minimap2 defaults in place.
     */
    ::bioseq::simd::HWY_NAMESPACE::Minimap2Params base_params{};

    /*
     * Effective per-scan persisted-payload read gate. `allow` preserves the
     * captured bioseq.read_sequence_index value; the two explicit contracts
     * override it. Payload probes happen later in the typed scanner path,
     * never in submit_scan.
     */
    bool read_sequence_index_active = false;
    bioseq::indexing::IndexReadRequirement index_read_requirement =
        bioseq::indexing::IndexReadRequirement::allow;

    /*
     * The OID of the relation being scanned. Only used to key into
     * bioseq_internal.index_sidecar; set at configure time so submit_scan
     * doesn't need to re-read RelationGetRelid per tuple.
     */
    Oid user_relid = InvalidOid;

    /*
     * Execution statistics captured during the last batch. Used by
     * BioseqColumnScan_ExplainCustomScan so it can compute the "Bioseq
     * Recomputed" percentage from runtime truth instead of the
     * plan-time estimate.
     */
    ::bioseq::pipeline_stats::PipelineExecutionStats last_stats{};

    std::vector<BufferedScanEntry> buffered_entries;
    std::map<int32_t, std::vector<::bioseq::simd::HWY_NAMESPACE::MatchResult>> match_map;
    std::vector<OutputCursor> output_cursors;
    int output_position = -1;

    bool join_mode = false;
    AttrNumber join_target_storage_seq_attno = InvalidAttrNumber;
    AttrNumber join_target_slot_seq_resno = InvalidAttrNumber;
    AttrNumber join_target_slot_rowno_resno = InvalidAttrNumber;
    AttrNumber join_query_slot_seq_resno = InvalidAttrNumber;
    std::vector<JoinQueryRef> join_queries{};
    std::map<std::uint64_t,
        std::vector<::bioseq::simd::HWY_NAMESPACE::MatchResult>>
        join_match_map{};
    std::vector<JoinOutputCursor> join_output_cursors{};
    std::size_t join_output_position = 0;
    bool join_target_active = false;
    bool join_target_prepared = false;
    bool join_target_outcomes_recorded = false;
	bool join_prepared_union_outcomes_recorded = false;
	bool join_prepared_build_checksum_recorded = false;
	::bioseq::simd::HWY_NAMESPACE::BatchProcessor::CpuPreparedTarget
		join_cpu_prepared_target{};
    std::array<const pgseq_t*, kDispatchBatchCapacity> join_target_hdrs{};
    std::array<const pgseq_t*, kDispatchBatchCapacity> join_query_hdrs{};
    std::array<int32_t, kDispatchBatchCapacity> join_query_ids{};
    std::array<pgseq_t*, kDispatchBatchCapacity> join_query_detoast_copies{};
    std::unique_ptr<
        ::bioseq::simd::HWY_NAMESPACE::BatchProcessor::ScannerPgContext>
        join_scanner_pg_ctx{};
    // PG_TRY is sigsetjmp-based: the catch path must not read a cache pointer
    // automatic that was assigned inside the protected region.
    bioseq::storage::seq_caches* join_scanner_pg_cache = nullptr;
    std::map<int32_t,
        std::vector<::bioseq::simd::HWY_NAMESPACE::MatchResult>>
        join_result_scratch{};

    /*
     * Additional Const-only mmsearch quals that share the primary
     * mmsearch_context's seq column and (alphabet, width) typmod.
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
     * DOC0260 §9 — per-bucket deferred-hydration machinery. The custom scan
     * runs with defer_seq_hydration=true, so submit_scan captures only
     * locators; run_batch_and_build_cursors groups the buffered locators by
     * bucket_idx and issues one fetch_payload_bulk per non-empty bucket
     * (each opens that bucket's shards once, scanning them row-number-sorted).
     * Hydrated payloads live in batch_payload_ctx (reset per batch); the
     * per-scan db/spc/relfilenode identity is captured by configure_params.
     */
    using SM = bioseq::storage::bioseq_default_storage_manager;
    Oid user_main_database_oid   = InvalidOid;
    Oid user_main_tablespace_oid = InvalidOid;
    Oid user_main_relfilenode    = InvalidOid;
    // Borrowed from the owning executor state for active scan execution. The
    // owner keeps it registered until every batch/scanner is drained; buffer
    // destruction never dereferences it and never changes its registration.
    Snapshot scan_snapshot       = nullptr;
    MemoryContext batch_payload_ctx = nullptr;
    std::array<std::vector<SM::fetch_request>, SM::bucket_count>
        per_bucket_target_requests{};
    std::array<std::vector<SM::fetch_request>, SM::bucket_count>
        per_bucket_query_requests{};
    std::vector<SM::fetch_request> fetch_bulk_sort_scratch{};
    std::vector<pgseq_t*>          hydrated_target_payloads{};
    std::vector<pgseq_t*>          hydrated_query_payloads{};
    // Member storage is intentional: PG_TRY/PG_CATCH uses setjmp/longjmp, so
    // a modified automatic optional would have an indeterminate catch value.
    std::optional<bioseq::parallelism::MemoryPermit> active_wave_permit{};
    /* §9.6 EXPLAIN counters. */
    std::uint64_t explain_bucket_touched_mask      = 0;
    std::uint64_t explain_storage_bulk_fetch_calls = 0;

    static void require_registered_mvcc_snapshot_(Snapshot snapshot) {
        if (snapshot == nullptr
            || !IsMVCCSnapshot(snapshot)
            || snapshot->regd_count == 0) {
            ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("bioseq dispatch requires a registered MVCC executor snapshot")));
        }
    }

public:
    explicit TupleDispatchBuffer(const size_t estimated_tuple_count) {
        buffered_entries.reserve(estimated_tuple_count * 2);
        output_cursors.reserve(estimated_tuple_count * 2);
    }

    /*
     * Install the per-scan aligner parameters. Called from
     * BioseqColumnScan_BeginCustomScan after the GUC snapshot + any per-call
     * aligner_options() overrides have been applied. Must be called before
     * the first submit_scan.
     */
    void configure_params(const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& params,
                          bioseq::indexing::IndexReadRequirement read_requirement,
                          bool read_sequence_index,
                          Oid scan_user_relid,
                          Oid scan_user_database_oid,
                          Oid scan_user_tablespace_oid,
                          Oid scan_user_main_relfilenode,
                          Snapshot executor_snapshot) {
        require_registered_mvcc_snapshot_(executor_snapshot);
        base_params                  = params;
        index_read_requirement       = read_requirement;
        read_sequence_index_active   =
            read_requirement
                    == bioseq::indexing::IndexReadRequirement::require_absent
                ? false
                : read_requirement
                    == bioseq::indexing::IndexReadRequirement::
                        require_compatible_persisted
                ? true
                : read_sequence_index;
        user_relid                   = scan_user_relid;
        user_main_database_oid       = scan_user_database_oid;
        user_main_tablespace_oid     = scan_user_tablespace_oid;
        user_main_relfilenode        = scan_user_main_relfilenode;
        scan_snapshot                = executor_snapshot;
        last_stats                   = ::bioseq::pipeline_stats::PipelineExecutionStats{};
        /*
         * DOC0260 §9.4 — the payload arena is parented on CurrentMemoryContext
         * at BeginCustomScan time (the executor per-query context), so the
         * bulk-fetched pgseq_t* survive the full scan, not just one tuple.
         */
        if (batch_payload_ctx == nullptr) {
            batch_payload_ctx = AllocSetContextCreate(CurrentMemoryContext,
                "TupleDispatchBuffer payload arena", ALLOCSET_DEFAULT_SIZES);
        }
        if (fetch_bulk_sort_scratch.size() < kDispatchBatchCapacity) {
            fetch_bulk_sort_scratch.resize(kDispatchBatchCapacity);
        }
    }

    // Parallel workers restore the leader's serialized scan snapshot only
    // after BeginCustomScan configured this buffer. Rebind the borrowed
    // pointer before the worker can dispatch any row; ownership remains with
    // BioseqScanState.
    void replace_executor_snapshot(Snapshot executor_snapshot) {
        require_registered_mvcc_snapshot_(executor_snapshot);
        scan_snapshot = executor_snapshot;
    }

    [[nodiscard]] const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& base_aligner_params() const { return base_params; }
    [[nodiscard]] const ::bioseq::pipeline_stats::PipelineExecutionStats& execution_stats() const { return last_stats; }
    [[nodiscard]] bool reads_sequence_index() const { return read_sequence_index_active; }

    /* DOC0260 §9.6 — per-scan storage-bucket EXPLAIN counters. */
    [[nodiscard]] std::uint64_t explain_bucket_touched_bits() const {
        return explain_bucket_touched_mask;
    }
    [[nodiscard]] std::uint64_t explain_bulk_fetch_calls() const {
        return explain_storage_bulk_fetch_calls;
    }

    /*
     * Record the mmsearch column / pk / query triple extracted from the
     * plan. Called once per scan from BioseqColumnScan_BeginCustomScan.
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
                            AttrNumber primary_key_attno) {
        mmsearch_context.seq_attno          = seq_attno;
        mmsearch_context.primary_key_attno  = primary_key_attno;
        mmsearch_context.column_name        = std::move(column_name);
        mmsearch_context.query_varlena      = std::move(query_varlena_bytes);
        mmsearch_context.alphabet_code      = alphabet_code;
        mmsearch_context.storage_unit_byte_width         = storage_unit_byte_width;
        mmsearch_context.coalesce_active    = false;
        mmsearch_context.coalesced_query_varlena.clear();
        mmsearch_context.recompute_due_to_alphabet_mismatch = false;
        mmsearch_context.per_row_query_attno = InvalidAttrNumber;
        mmsearch_context.per_row_self_alignment = false;
        /* Capture the raw query's typmod from the header (best-effort: the
         * header may be empty if the caller passed bogus bytes, in which
         * case the configure_mmsearch fast-path already logs and skips.) */
        if (!mmsearch_context.query_varlena.empty()) {
            const pgseq_t* hdr =
                reinterpret_cast<const pgseq_t*>(
                    mmsearch_context.query_varlena.data());
            mmsearch_context.raw_query_alphabet_code = hdr->alphabet_code;
            mmsearch_context.raw_query_storage_unit_byte_width    = hdr->storage_unit_byte_width;
        } else {
            mmsearch_context.raw_query_alphabet_code = alphabet_code;
            mmsearch_context.raw_query_storage_unit_byte_width    = storage_unit_byte_width;
        }
        mmsearch_context.active             = true;

        uint32_t query_bp_size = 0;
        if (const auto* hdr = mmsearch_context.query_hdr()) {
            query_bp_size = hdr->bp_size;
        }
        bioseq_log<DBG>("[bioseq] pg: TupleDispatchBuffer::configure_mmsearch() seq_attno={} pk_attno={} column='{}' typmod=({},{}) query_bp_size={}",
                static_cast<int>(seq_attno),
                static_cast<int>(primary_key_attno),
                mmsearch_context.column_name,
                static_cast<unsigned>(alphabet_code),
                static_cast<unsigned>(storage_unit_byte_width),
                query_bp_size);
    }

    /*
     * Configure the per-row-query mode. Called by
     * BioseqColumnScan_BeginCustomScan when the mmsearch second arg is a
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
                                    AttrNumber primary_key_attno) {
        mmsearch_context.seq_attno              = seq_attno;
        mmsearch_context.primary_key_attno      = primary_key_attno;
        mmsearch_context.column_name            = std::move(column_name);
        mmsearch_context.query_varlena.clear();
        mmsearch_context.coalesced_query_varlena.clear();
        mmsearch_context.coalesce_active        = false;
        mmsearch_context.alphabet_code          = alphabet_code;
        mmsearch_context.storage_unit_byte_width             = storage_unit_byte_width;
        mmsearch_context.raw_query_alphabet_code = alphabet_code;
        mmsearch_context.raw_query_storage_unit_byte_width    = storage_unit_byte_width;
        mmsearch_context.per_row_query_attno    = per_row_query_attno;
        mmsearch_context.per_row_self_alignment = per_row_self_alignment;
        mmsearch_context.recompute_due_to_alphabet_mismatch = false;
        mmsearch_context.active                 = true;

        bioseq_log<DBG>("[bioseq] pg: TupleDispatchBuffer::configure_mmsearch_per_row() seq_attno={} per_row_query_attno={} self_alignment={} typmod=({},{})",
                static_cast<int>(seq_attno),
                static_cast<int>(per_row_query_attno),
                per_row_self_alignment,
                static_cast<unsigned>(alphabet_code),
                static_cast<unsigned>(storage_unit_byte_width));
    }

    /* Configure the narrow two-child BioseqBatchJoin seam. */
    void configure_mmsearch_join(
            const Oid target_relation_oid,
            const AttrNumber target_storage_seq_attno,
            const AttrNumber target_slot_seq_resno,
            const AttrNumber target_slot_rowno_resno,
            const AttrNumber query_slot_seq_resno,
            std::string target_column_name,
            const std::uint8_t alphabet_code,
            const std::uint8_t storage_unit_byte_width) {
        if (!OidIsValid(target_relation_oid)
            || target_storage_seq_attno <= 0
            || target_slot_seq_resno <= 0
            || target_slot_rowno_resno <= 0
            || query_slot_seq_resno <= 0) {
            ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("BioseqBatchJoin received invalid relation/attribute metadata")));
        }
        if (OidIsValid(user_relid) && user_relid != target_relation_oid) {
            ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("BioseqBatchJoin target relation disagrees with dispatch configuration")));
        }
        user_relid = target_relation_oid;
        join_mode = true;
        join_target_storage_seq_attno = target_storage_seq_attno;
        join_target_slot_seq_resno = target_slot_seq_resno;
        join_target_slot_rowno_resno = target_slot_rowno_resno;
        join_query_slot_seq_resno = query_slot_seq_resno;
        mmsearch_context = MmsearchContext{};
        mmsearch_context.seq_attno = target_slot_seq_resno;
        mmsearch_context.primary_key_attno = InvalidAttrNumber;
        mmsearch_context.column_name = std::move(target_column_name);
        mmsearch_context.alphabet_code = alphabet_code;
        mmsearch_context.storage_unit_byte_width = storage_unit_byte_width;
        mmsearch_context.raw_query_alphabet_code = alphabet_code;
        mmsearch_context.raw_query_storage_unit_byte_width = storage_unit_byte_width;
        mmsearch_context.active = true;
        reset_join_runtime_for_rescan();
    }

    /* Pin/copy one locator-passthrough target for all query batches. */
    void begin_join_target(TupleTableSlot* target_slot) {
        if (!join_mode || target_slot == nullptr || !join_queries.empty()) {
            ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("BioseqBatchJoin target lifecycle is invalid")));
        }
        bool sequence_is_null = true;
        const Datum sequence_datum = slot_getattr(
            target_slot, join_target_slot_seq_resno, &sequence_is_null);
        if (sequence_is_null) {
            ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("BioseqBatchJoin locator target sequence is NULL")));
        }
        auto target_sequence = bioseq::pg_utils::detoast_as<pgseq_t>(
            bioseq::pg_utils::pg_datum::from_raw(sequence_datum));
        const bool is_locator =
            bioseq::storage::is_locator_stub(target_sequence.ptr);
        target_sequence.free_if_copy();
        if (!is_locator) {
            ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("BioseqBatchJoin target provider did not return a locator stub"),
                 errdetail("locator passthrough is mandatory; materialized target values are not accepted by this executor")));
        }
        clear_slots();
        submit_scan(target_slot);
        if (buffered_entries.size() != 1) {
            ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("BioseqBatchJoin failed to retain exactly one target")));
        }
        bool rowno_is_null = true;
        const Datum rowno_datum = slot_getattr(
            target_slot, join_target_slot_rowno_resno, &rowno_is_null);
        if (rowno_is_null) {
            ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("BioseqBatchJoin hidden target row number is NULL")));
        }
        const std::int64_t rowno = DatumGetInt64(rowno_datum);
        if (rowno < 0) {
            ereport(ERROR,
                (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                 errmsg("BioseqBatchJoin hidden target row number is negative")));
        }
        buffered_entries.front().sequence_rowno = rowno;
        join_target_active = true;
        join_target_prepared = false;
        join_target_outcomes_recorded = false;
		join_prepared_union_outcomes_recorded = false;
		join_prepared_build_checksum_recorded = false;
		join_cpu_prepared_target =
			::bioseq::simd::HWY_NAMESPACE::BatchProcessor::CpuPreparedTarget{};
		join_cpu_prepared_target.target_rowno = rowno;
    }

    void prepare_join_target_queries(std::span<const JoinQueryRef> all_queries) {
        if (!join_target_active || join_target_prepared) {
            ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("BioseqBatchJoin target preparation is out of order")));
        }
        if (base_params.vulkan_enabled) {
            ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("Vulkan BioseqBatchJoin locator execution remains fail closed"),
                 errdetail("CPU prepared-target construction is forbidden for a Vulkan-selected statement")));
        }
        std::vector<const pgseq_t*> query_headers;
        std::vector<pgseq_t*> detoast_copies;
        query_headers.reserve(all_queries.size());
        detoast_copies.reserve(all_queries.size());
        std::uint64_t previous_ordinal = 0;
        bool first = true;
        for (const JoinQueryRef& query : all_queries) {
            if (query.query_slot == nullptr
                || query.ordinal
                    > static_cast<std::uint64_t>((std::numeric_limits<std::int32_t>::max)())
                || (!first && query.ordinal <= previous_ordinal)) {
                ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("BioseqBatchJoin query references are invalid or unordered")));
            }
            bool query_is_null = true;
            const Datum query_datum = slot_getattr(
                query.query_slot, join_query_slot_seq_resno, &query_is_null);
            if (query_is_null) {
                ereport(ERROR,
                    (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                     errmsg("BioseqBatchJoin query sequence is NULL")));
            }
			pgseq_t* query_header = reinterpret_cast<pgseq_t*>(
				PG_DETOAST_DATUM(query_datum));
			if (bioseq::storage::is_locator_stub(query_header)) {
				if (reinterpret_cast<void*>(query_header)
					!= reinterpret_cast<void*>(DatumGetPointer(query_datum))) {
					pfree(query_header);
				}
				ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("BioseqBatchJoin query preparation received a locator stub")));
			}
			if (query_header->alphabet_code != mmsearch_context.alphabet_code
				|| query_header->storage_unit_byte_width
					!= mmsearch_context.storage_unit_byte_width) {
				if (reinterpret_cast<void*>(query_header)
					!= reinterpret_cast<void*>(DatumGetPointer(query_datum))) {
					pfree(query_header);
				}
				ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("BioseqBatchJoin query typmod changed during preparation")));
			}
			query_headers.push_back(query_header);
			if (reinterpret_cast<void*>(query_header)
				!= reinterpret_cast<void*>(DatumGetPointer(query_datum))) {
				detoast_copies.push_back(query_header);
			}
            previous_ordinal = query.ordinal;
            first = false;
        }
		const BufferedScanEntry& target = buffered_entries.front();
		const std::uint64_t target_bp_size = target.seq_loc.has_value()
			? static_cast<std::uint64_t>(target.seq_loc->total_bp_size)
			: (target.seq_hdr() == nullptr
				? 0u : static_cast<std::uint64_t>(target.seq_hdr()->bp_size));
		if (read_sequence_index_active) {
			MemoryContext saved_context = CurrentMemoryContext;
			PG_TRY();
			{
				processor.prepare_cpu_shared_query_unions(
					mmsearch_context.alphabet_code,
					mmsearch_context.storage_unit_byte_width,
					target_bp_size,
					query_headers,
					base_params,
					/*include_persisted_parameter_group=*/true,
					join_cpu_prepared_target);
			}
			PG_CATCH();
			{
				MemoryContextSwitchTo(saved_context);
				for (pgseq_t* copy : detoast_copies) {
					pfree(copy);
				}
				PG_RE_THROW();
			}
			PG_END_TRY();
		} else {
			/* no_index must retain the requested all-SIMD nthash pipeline.
			 * Persistent builders currently expose only Scalar/Vulkan target
			 * sketch backends, so constructing a CPU prepared index here would be
			 * a silent scalar fallback and a critical acceptance failure. */
			join_cpu_prepared_target.target_bp_size = target_bp_size;
			join_cpu_prepared_target.query_count = query_headers.size();
			join_cpu_prepared_target.unions_ready = false;
			bioseq_log<DBG>("[bioseq] pg: BatchJoin no_index prepared target disabled: ExactSimd target-sketch backend is unavailable");
		}
		for (pgseq_t* copy : detoast_copies) {
			pfree(copy);
		}
        join_target_prepared = true;
    }

    void set_join_query_batch(std::span<const JoinQueryRef> queries) {
        if (!join_target_prepared
            || join_output_position != join_output_cursors.size()) {
            ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("BioseqBatchJoin query batch lifecycle is invalid")));
        }
        join_output_cursors.clear();
        join_match_map.clear();
        if (queries.size() > kDispatchBatchCapacity) {
            ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("BioseqBatchJoin query batch exceeds dispatch capacity")));
        }
        join_queries.assign(queries.begin(), queries.end());
        join_output_position = 0;
    }

    /*
     * Re-encode the literal query under (target_alphabet,
     * target_width) so the BatchProcessor's static dispatch agrees
     * with the column's typmod. Caller passes the detoasted bytes
     * of the re-encoded value (typically built via the same
     * convert_from(cstr, …, pg_alloc) path used by seq_in / seq_cast).
     *
     * The ORIGINAL query_varlena is preserved (so EXPLAIN can
     * surface the literal the user wrote) and `query_hdr()`
     * returns the coalesced bytes whenever `coalesce_active` is
     * true.
     */
    void apply_typmod_coalesce(std::vector<char> coalesced_bytes) {
        mmsearch_context.coalesced_query_varlena = std::move(coalesced_bytes);
        mmsearch_context.coalesce_active = true;
        bioseq_log<DBG>("[bioseq] pg: TupleDispatchBuffer::apply_typmod_coalesce() raw=({},{}) coalesced=({},{}) raw_bytes={} coalesced_bytes={}",
                static_cast<unsigned>(mmsearch_context.raw_query_alphabet_code),
                static_cast<unsigned>(mmsearch_context.raw_query_storage_unit_byte_width),
                static_cast<unsigned>(mmsearch_context.alphabet_code),
                static_cast<unsigned>(mmsearch_context.storage_unit_byte_width),
                mmsearch_context.query_varlena.size(),
                mmsearch_context.coalesced_query_varlena.size());
    }

    /*
     * Toggle: in cross-alphabet recompute mode the run-batch path
     * MUST recompute minimizers for every target. Sidecar blobs are
     * sketched under the column's alphabet, but the alignment now
     * runs against a query that lives in a different alphabet —
     * minimizer hashes are not cross-alphabet compatible. Setting
     * this disables the index-blob fast path until the next
     * configure_mmsearch.
     */
    void mark_alphabet_mismatch_recompute() {
        mmsearch_context.recompute_due_to_alphabet_mismatch = true;
        bioseq_log<DBG>("[bioseq] pg: TupleDispatchBuffer::mark_alphabet_mismatch_recompute() set");
    }

    [[nodiscard]] bool per_row_mode() const {
        return mmsearch_context.active
            && mmsearch_context.per_row_query_attno != InvalidAttrNumber;
    }
    [[nodiscard]] AttrNumber per_row_query_attno() const {
        return mmsearch_context.per_row_query_attno;
    }
    [[nodiscard]] bool coalesce_active() const {
        return mmsearch_context.coalesce_active;
    }
    [[nodiscard]] bool recompute_due_to_alphabet_mismatch() const {
        return mmsearch_context.recompute_due_to_alphabet_mismatch;
    }
    [[nodiscard]] bool per_row_self_alignment() const {
        return mmsearch_context.per_row_self_alignment;
    }
    [[nodiscard]] uint8_t mmsearch_alphabet_code() const {
        return mmsearch_context.alphabet_code;
    }
    [[nodiscard]] uint8_t mmsearch_storage_unit_byte_width() const {
        return mmsearch_context.storage_unit_byte_width;
    }
    [[nodiscard]] uint8_t raw_query_alphabet_code() const {
        return mmsearch_context.raw_query_alphabet_code;
    }
    [[nodiscard]] uint8_t raw_query_storage_unit_byte_width() const {
        return mmsearch_context.raw_query_storage_unit_byte_width;
    }

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
     * `clear_extras=true` is the historical full-reset semantics
     * (used by BeginCustomScan defensive init only). Default is
     * false so ReScanCustomScan and any other caller preserves the
     * extras list across rebinds.
     */
    void clear_mmsearch_context(bool clear_extras = false) {
        mmsearch_context = MmsearchContext{};
        if (clear_extras) {
            extra_query_varlenas.clear();
        }
    }

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
    void add_extra_const_query(std::vector<char> query_varlena_bytes) {
        const pgseq_t* hdr =
            reinterpret_cast<const pgseq_t*>(
                query_varlena_bytes.data());
        const uint32_t bp_size = (query_varlena_bytes.empty() || hdr == nullptr)
            ? 0u : hdr->bp_size;
        bioseq_log<DBG>("[bioseq] pg: TupleDispatchBuffer::add_extra_const_query() bp_size={} (extra index #{})",
                bp_size, extra_query_varlenas.size());
        extra_query_varlenas.push_back(std::move(query_varlena_bytes));
    }

    [[nodiscard]] size_t extra_query_count() const {
        return extra_query_varlenas.size();
    }

    /*
     * Writing-phase hook. Fired from Inner_CustomExecScan after ExecQual
     * passes, using econtext->ecxt_scantuple which still holds every
     * attribute of the base relation. Extracts the bioseq.seq column
     * named by configure_mmsearch (detoasting its varlena), the PK
     * column value, and copies the slot into a MinimalTuple-backed
     * clone so the data outlives the next ExecScanFetch call.
     */
    [[gnu::always_inline]] inline void submit_scan(TupleTableSlot* scan_slot) {
        if (!mmsearch_context.active) {
            bioseq_log<DBG>("[bioseq] pg: submit_scan called while mmsearch_context inactive; skipping");
            return;
        }
        if (scan_slot == nullptr) {
            bioseq_log<DBG>("[bioseq] pg: submit_scan received null scan_slot; skipping");
            return;
        }
        if (buffered_entries.size() >= kDispatchBatchCapacity) {
            ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("bioseq dispatch buffer capacity invariant was violated"),
                 errdetail("buffered=%zu capacity=%zu",
                    buffered_entries.size(), kDispatchBatchCapacity)));
        }

        BufferedScanEntry entry{};

        /*
         * Capture the scan slot's tts_tid (set by the bioseq columnar
         * access method in BioseqColumnScanNext) and invert it back to a
         * row number so downstream consumers can join against the
         * sidecar tables (keyed on (user_relid, sequence_rowno)). Uses
         * the canonical tid_to_row_number helper from access_method.cppm
         * so the encoding stays in lockstep with row_number_to_tid.
         */
        if (ItemPointerIsValid(&scan_slot->tts_tid)) {
            entry.sequence_rowno = static_cast<int64_t>(
                tid_to_row_number(scan_slot->tts_tid));
        }

        /* Extract the target bioseq.seq column (mmsearch arg0). */
        if (mmsearch_context.seq_attno == InvalidAttrNumber) {
            entry.seq_is_null = true;
        } else {
            bool is_null = true;
            Datum seq_datum = slot_getattr(scan_slot, mmsearch_context.seq_attno, &is_null);
            if (is_null) {
                entry.seq_is_null = true;
            } else {
                /*
                 * PG_DETOAST_DATUM resolves short-header/TOASTed/compressed
                 * varlenas into a flat 4-byte-header copy we can reinterpret
                 * as pgseq_t*. The copy lives in
                 * CurrentMemoryContext, which is the per-query context here
                 * — it will be reset long after we finish writing, so the
                 * read below is safe. We still memcpy the bytes into our
                 * std::vector<char> so the BufferedScanEntry survives
                 * ResetExprContext / per-tuple context cleanup between
                 * calls to ExecScanFetch.
                 */
	                auto src = bioseq::pg_utils::detoast_as<pgseq_t>(
	                    bioseq::pg_utils::pg_datum::from_raw(seq_datum));
	                pgseq_t* src_hdr = src.ptr;
                if (bioseq::storage::is_locator_stub(src_hdr)) {
                    /*
                     * DOC0260 §9.2 — defer_seq_hydration path: capture the
                     * locator only; run_batch_and_build_cursors bulk-fetches
                     * the payload per bucket. seq_varlena stays empty.
                     */
                    entry.seq_loc =
                        bioseq::storage::decode_locator_from_stub(src_hdr);
                    entry.seq_is_null = false;
                } else {
                    const size_t total_bytes = static_cast<size_t>(VARSIZE(src_hdr));
                    entry.seq_varlena.assign(
                        reinterpret_cast<const char*>(src_hdr),
                        reinterpret_cast<const char*>(src_hdr) + total_bytes);
                    entry.seq_is_null = false;
                }
	                src.free_if_copy();
            }
        }

        /* Extract the primary-key column. */
        if (mmsearch_context.primary_key_attno == InvalidAttrNumber) {
            entry.primary_key_is_null = true;
        } else {
            bool pk_null = true;
            Datum pk_datum = slot_getattr(scan_slot, mmsearch_context.primary_key_attno, &pk_null);
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
         * Per-row-query bytes carry the column's typmod (no coalesce
         * needed here because both arg0 and arg1 are Vars of the same
         * relation, so both inherit the same column typmod).
         */
        if (mmsearch_context.per_row_query_attno != InvalidAttrNumber) {
            bool query_null = true;
            Datum query_datum = slot_getattr(scan_slot,
                mmsearch_context.per_row_query_attno, &query_null);
            if (query_null) {
                entry.per_row_query_is_null = true;
            } else {
	                auto src = bioseq::pg_utils::detoast_as<pgseq_t>(
	                    bioseq::pg_utils::pg_datum::from_raw(query_datum));
	                pgseq_t* src_hdr = src.ptr;
                if (bioseq::storage::is_locator_stub(src_hdr)) {
                    /* DOC0260 §9.2 — per-row query locator (bulk-fetched). */
                    entry.per_row_query_loc =
                        bioseq::storage::decode_locator_from_stub(src_hdr);
                    entry.per_row_query_is_null = false;
                } else {
                    const size_t total_bytes = static_cast<size_t>(VARSIZE(src_hdr));
                    entry.per_row_query_varlena.assign(
                        reinterpret_cast<const char*>(src_hdr),
                        reinterpret_cast<const char*>(src_hdr) + total_bytes);
                    entry.per_row_query_is_null = false;
                }
	                src.free_if_copy();
            }
        }

        /*
         * Force materialization of every attribute before cloning. The
         * columnar scan only populates tts_values[] for attributes in
         * attrNeededList; anything outside that set carries a stale
         * Datum. ExecCopySlotMinimalTuple serialises tts_values[] to a
         * MinimalTuple, and slot_getallattrs() guarantees every entry
         * is valid before serialisation.
         */
        slot_getallattrs(scan_slot);

        entry.scan_slot_copy = MakeSingleTupleTableSlot(
            scan_slot->tts_tupleDescriptor, &TTSOpsMinimalTuple);
        ExecStoreMinimalTuple(ExecCopySlotMinimalTuple(scan_slot),
                              entry.scan_slot_copy, /*shouldFree=*/true);

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

        /*
         * DOC0279 §11.3c — no per-row blob read here any more. The stored
         * index (if any) is ASSEMBLED at batch time from its chunked storage,
         * keyed by entry.sequence_rowno (the columnar rowno == index
         * query_key, already computed above), so submit_scan only needs the
         * rowno (carried into scanner_pg_ctx.target_rownos[]).
         */
        const uint32_t entry_seq_bp_size = entry.seq_hdr() != nullptr
            ? entry.seq_hdr()->bp_size : 0;
        bioseq_log<DBG>("[bioseq] pg: submit_scan pk={} rowno={} seq_bp_size={} (null={})",
                entry.primary_key_is_null ? -1 : entry.primary_key,
                entry.sequence_rowno,
                entry_seq_bp_size,
                entry.seq_is_null);

        buffered_entries.push_back(std::move(entry));
    }

    /* True exactly when the current physical buffer must be dispatched. */
    [[nodiscard]] bool ready_to_dispatch() const noexcept {
        return buffered_entries.size() == kDispatchBatchCapacity;
    }

    /*
     * DOC0260 §9.3 — per-bucket bulk hydration of one seq column. Groups the
     * buffered entries that captured a locator for `column_attno` by
     * bucket_idx and issues one SM::fetch_payload_bulk per non-empty bucket
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
            std::array<std::vector<SM::fetch_request>, SM::bucket_count>& per_bucket,
            std::vector<pgseq_t*>& out_payloads,
            bool is_query) {
        for (auto& v : per_bucket) {
            v.clear();
        }
        // DOC0274 — compute letters_per_unit + storage_unit_byte_width from
        // the batch's (alphabet, width) typmod so the MaxAllocSize gate below
        // works for ANY alphabet / storage_unit_byte_width pair (dna4/uint8,
        // dna5/uint64, aa20/uint32, …) — NOT just the 2-bits-per-letter dna4
        // case. The dispatch happens once per column (not per row).
        //
        // Explicit captures (no `[&]` capture-all) per DOC0264 R6 — the user-
        // visible rule on the hot path is no capture-all lambdas.
        std::size_t lpu_for_column          = 0;
        std::size_t bytes_per_unit_for_col  = 0;
        pgseq_t::dispatch_instance(
            mmsearch_context.alphabet_code,
            mmsearch_context.storage_unit_byte_width,
            [&lpu_for_column, &bytes_per_unit_for_col]<class S>() noexcept {
                using traits = typename compact_sequence_range<S>::traits;
                lpu_for_column         = traits::letters_per_unit;
                bytes_per_unit_for_col = sizeof(typename traits::storage_unit);
            });
        if (lpu_for_column == 0 || bytes_per_unit_for_col == 0) {
            bioseq_log<ERR, PG>(
                "hydrate_one_column: dispatch_instance returned zero lpu / "
                "bytes_per_unit for (alphabet={}, width={})",
                static_cast<unsigned>(mmsearch_context.alphabet_code),
                static_cast<unsigned>(mmsearch_context.storage_unit_byte_width));
        }
        const size_t batch_size = buffered_entries.size();
        bool any = false;
        for (const size_t i : rows) {
            if (i >= batch_size) {
                continue;
            }
            const auto& loc = is_query ? buffered_entries[i].per_row_query_loc
                                       : buffered_entries[i].seq_loc;
            if (!loc.has_value()) {
                continue;
            }
            // DOC0274 — Phase C / D-VARLENA bridge: skip hydration when the
            // assembled varlena would exceed MaxAllocSize. The scanner_pg
            // dispatch path reads such sequences fragment-by-fragment without
            // materialising. Projection of `SELECT seq` on these rows raises
            // bioseq_log<ERR, PG>() from access_method.cppm — that's the documented
            // Phase C limit until Phase D D-VARLENA lifts it.
            //
            // Size = ceil(total_bp / letters_per_unit) * bytes_per_unit + header,
            // where lpu and bytes_per_unit come from the column's (alphabet,
            // width) typmod — generic across all supported alphabets.
            const std::uint64_t total_units = lpu_for_column == 0
                ? std::uint64_t{0}
                : (static_cast<std::uint64_t>(loc->total_bp_size)
                       + lpu_for_column - 1)
                  / lpu_for_column;
            const std::uint64_t projected_payload_bytes =
                total_units * static_cast<std::uint64_t>(bytes_per_unit_for_col);
            const std::uint64_t projected_varlena =
                static_cast<std::uint64_t>(pgseq_t::header_bytes())
                + projected_payload_bytes;
            if (projected_varlena > static_cast<std::uint64_t>(MaxAllocSize)) {
                bioseq_log<DBG>("[bioseq] pg: hydrate_one_column row={} attno={} too large "
                        "({} bp, lpu={}, byte_width={} ⇒ {} bytes > MaxAllocSize); "
                        "deferring to scanner_pg",
                        i, static_cast<int>(column_attno),
                        loc->total_bp_size, lpu_for_column,
                        bytes_per_unit_for_col, projected_varlena);
                continue;
            }
            per_bucket[loc->bucket_idx].push_back(SM::fetch_request{i, *loc});
            any = true;
        }
        if (!any) {
            return;
        }
        out_payloads.assign(batch_size, nullptr);
        for (std::uint8_t b = 0; b < SM::bucket_count; ++b) {
            auto& reqs = per_bucket[b];
            if (reqs.empty()) {
                continue;
            }
            SM::fetch_payload_bulk(
                user_main_database_oid, user_main_tablespace_oid,
                user_main_relfilenode, column_attno, static_cast<std::int16_t>(b),
                std::span<const SM::fetch_request>{reqs.data(), reqs.size()},
                std::span<pgseq_t*>{out_payloads.data(), out_payloads.size()},
                std::span<SM::fetch_request>{fetch_bulk_sort_scratch.data(),
                                             fetch_bulk_sort_scratch.size()},
                scan_snapshot, batch_payload_ctx, cache);
            explain_bucket_touched_mask |= (std::uint64_t{1} << b);
            ++explain_storage_bulk_fetch_calls;
        }
        for (const size_t i : rows) {
            if (i >= batch_size) {
                continue;
            }
            if (out_payloads[i] == nullptr) {
                continue;
            }
            if (is_query) {
                buffered_entries[i].hydrated_per_row_query_hdr = out_payloads[i];
            } else {
                buffered_entries[i].hydrated_seq_hdr = out_payloads[i];
            }
	            TupleTableSlot* sslot = buffered_entries[i].scan_slot_copy;
	            if (sslot != nullptr) {
	                const int idx = column_attno - 1;
	                if (idx >= 0 && idx < sslot->tts_tupleDescriptor->natts) {
	                    bioseq::pg_utils::pg_slot_datums_view{sslot}.set_raw(
	                        column_attno,
	                        bioseq::pg_utils::pg_datum::from_pointer(out_payloads[i]));
	                }
	            }
        }
    }

    /*
     * DOC0260 §9.3 — entry point for per-bucket bulk hydration. Allocates a
     * short-lived read-side seq_caches (its open shard relations are released
     * by close_all before destroy), runs the target column and, when present,
     * the per-row query column. A no-op when no entry captured a locator
     * (e.g. an inline non-stub value, or no mmsearch column).
     *
     * DOC0264 §9.4 — Phase C: fetch_payload now stitches per-fragment rows
     * into one contiguous varlena, bounded by MaxAllocSize. The dispatch
     * loop continues to use `with_lazy_in_memory` over the hydrated varlena
     * (Phase B baseline) — see DOC0273 §3.2 for the design trade-off: the
     * Phase C `sequence_scanner_pg<S>` + `with_lazy_postgres<R>` machinery
     * is built but not wired into the single-Scanner pipeline tree, to keep
     * dispatch_buffer.cppm under the DOC0271 template-explosion budget.
     * Hydration here also serves output projection (write-back into the
     * buffered slot's tts_values for `SELECT seq` resolution).
     */
    void hydrate_locators_by_bucket(std::span<const size_t> rows) {
        const size_t batch_size = buffered_entries.size();
        bool any_locator = false;
        for (const size_t i : rows) {
            if (i >= batch_size) {
                continue;
            }
            if (buffered_entries[i].seq_loc.has_value()
                || buffered_entries[i].per_row_query_loc.has_value()) {
                any_locator = true;
                break;
            }
        }
        if (!any_locator) {
            return;
        }
        if (batch_payload_ctx == nullptr) {
            batch_payload_ctx = AllocSetContextCreate(CurrentMemoryContext,
                "TupleDispatchBuffer payload arena", ALLOCSET_DEFAULT_SIZES);
        }
        if (fetch_bulk_sort_scratch.size() < batch_size) {
            fetch_bulk_sort_scratch.resize(batch_size);
        }

        bioseq::storage::seq_caches* cache =
            bioseq::storage::alloc_seq_caches(batch_payload_ctx);
        cache->side                = bioseq::storage::seq_caches::Side::Read;
        cache->lock_mode           = AccessShareLock;
        cache->read_state_mctx     = batch_payload_ctx;
        cache->main_database_oid   = user_main_database_oid;
        cache->main_tablespace_oid = user_main_tablespace_oid;
        cache->main_relfilenode    = user_main_relfilenode;

        hydrate_one_column(rows, cache,
            static_cast<std::int16_t>(mmsearch_context.seq_attno),
            per_bucket_target_requests, hydrated_target_payloads,
            /*is_query=*/false);

        if (mmsearch_context.per_row_query_attno != InvalidAttrNumber) {
            hydrate_one_column(rows, cache,
                static_cast<std::int16_t>(mmsearch_context.per_row_query_attno),
                per_bucket_query_requests, hydrated_query_payloads,
                /*is_query=*/true);
        }

        cache->close_all(/*commit=*/false);
        bioseq::storage::destroy_seq_caches(cache);
    }

    [[nodiscard]] bool run_join_query_batch_and_build_cursors() {
        if (!join_mode || !join_target_prepared || !join_target_active) {
            ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("BioseqBatchJoin query batch ran before target preparation")));
        }
        if (join_queries.empty()) {
            last_stats = ::bioseq::pipeline_stats::PipelineExecutionStats{};
            join_output_cursors.clear();
            join_output_position = 0;
            return false;
        }
        if (join_queries.size() > kDispatchBatchCapacity
            || buffered_entries.size() != 1) {
            ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("BioseqBatchJoin dispatch shape is invalid")));
        }

        bioseq::parallelism::pg::backend_sync();
        last_stats = ::bioseq::pipeline_stats::PipelineExecutionStats{};
        const std::size_t batch_size = join_queries.size();
        join_target_hdrs.fill(nullptr);
        join_query_hdrs.fill(nullptr);
        join_query_ids.fill(0);
        join_query_detoast_copies.fill(nullptr);
        join_result_scratch.clear();
        if (!join_scanner_pg_ctx) {
            join_scanner_pg_ctx = std::make_unique<
                ::bioseq::simd::HWY_NAMESPACE::BatchProcessor::
                    ScannerPgContext>();
        }
        *join_scanner_pg_ctx =
            ::bioseq::simd::HWY_NAMESPACE::BatchProcessor::
                ScannerPgContext{};
        const auto ticket = bioseq::parallelism::pg::begin_batch(
            static_cast<std::int64_t>(batch_size));
		auto prepared_counter_totals = [this] {
			std::array<std::uint64_t, 3> totals{};
			for (const auto& group : join_cpu_prepared_target.groups) {
				totals[0] += group.target_index_builds;
				totals[1] += group.target_index_reuses;
				totals[2] += group.duplicate_build_attempts;
			}
			return totals;
		};
		const auto prepared_before = prepared_counter_totals();
        join_scanner_pg_cache = nullptr;
        PG_TRY();
        {
            BufferedScanEntry& target = buffered_entries.front();
            for (std::size_t i = 0; i < batch_size; ++i) {
                const JoinQueryRef& query = join_queries[i];
                if (query.query_slot == nullptr
                    || query.ordinal
                        > static_cast<std::uint64_t>(
                            (std::numeric_limits<std::int32_t>::max)())) {
                    ereport(ERROR,
                        (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                         errmsg("BioseqBatchJoin query reference is invalid")));
                }
                bool query_is_null = true;
                const Datum query_datum = slot_getattr(
                    query.query_slot, join_query_slot_seq_resno, &query_is_null);
                if (query_is_null) {
                    ereport(ERROR,
                        (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                         errmsg("BioseqBatchJoin query sequence is NULL")));
                }
                pgseq_t* query_hdr = reinterpret_cast<pgseq_t*>(
                    PG_DETOAST_DATUM(query_datum));
                if (bioseq::storage::is_locator_stub(query_hdr)) {
                    ereport(ERROR,
                        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                         errmsg("BioseqBatchJoin query child returned a locator stub"),
                         errhint("The bounded query child must materialize its compact sequence before join dispatch.")));
                }
                if (query_hdr->alphabet_code != mmsearch_context.alphabet_code
                    || query_hdr->storage_unit_byte_width
                        != mmsearch_context.storage_unit_byte_width) {
                    ereport(ERROR,
                        (errcode(ERRCODE_DATATYPE_MISMATCH),
                         errmsg("BioseqBatchJoin query typmod changed after planning")));
                }
                if (reinterpret_cast<void*>(query_hdr)
                    != reinterpret_cast<void*>(DatumGetPointer(query_datum))) {
                    join_query_detoast_copies[i] = query_hdr;
                }
                join_query_hdrs[i] = query_hdr;
                join_target_hdrs[i] = target.seq_hdr();
                join_query_ids[i] = static_cast<std::int32_t>(query.ordinal);
            }

            if (batch_payload_ctx == nullptr) {
                batch_payload_ctx = AllocSetContextCreate(
                    CurrentMemoryContext,
                    "TupleDispatchBuffer join payload arena",
                    ALLOCSET_DEFAULT_SIZES);
            }
            join_scanner_pg_cache = bioseq::storage::alloc_seq_caches(
                batch_payload_ctx);
            join_scanner_pg_cache->side =
                bioseq::storage::seq_caches::Side::Read;
            join_scanner_pg_cache->lock_mode = AccessShareLock;
            join_scanner_pg_cache->read_state_mctx = batch_payload_ctx;
            join_scanner_pg_cache->main_database_oid = user_main_database_oid;
            join_scanner_pg_cache->main_tablespace_oid = user_main_tablespace_oid;
            join_scanner_pg_cache->main_relfilenode = user_main_relfilenode;

            auto& scanner_pg_ctx = *join_scanner_pg_ctx;
            scanner_pg_ctx.db_oid = user_main_database_oid;
            scanner_pg_ctx.tablespace_oid = user_main_tablespace_oid;
            scanner_pg_ctx.main_relfilenode = user_main_relfilenode;
            scanner_pg_ctx.mmsearch_seq_attno =
                static_cast<std::int16_t>(join_target_storage_seq_attno);
            scanner_pg_ctx.caches = join_scanner_pg_cache;
            scanner_pg_ctx.parent_mctx = batch_payload_ctx;
            scanner_pg_ctx.snapshot = scan_snapshot;
            scanner_pg_ctx.user_relid = user_relid;
            scanner_pg_ctx.index_read_requirement = index_read_requirement;
            scanner_pg_ctx.read_index_active = read_sequence_index_active;
            scanner_pg_ctx.index_async_persist_active =
                bioseq_index_async_persist;
            scanner_pg_ctx.recompute_async_persist_active =
                bioseq_recompute_async_persist;
            scanner_pg_ctx.active_index = std::string{
                bioseq::indexing::DEFAULT_ACTIVE_READ_INDEX};
            if (OidIsValid(user_relid)) {
                scanner_pg_ctx.active_index =
                    bioseq::storage::indexing::resolve_active_read_index(
                        user_relid,
                        user_main_relfilenode,
                        static_cast<std::int16_t>(
                            join_target_storage_seq_attno));
            }
            const bool record_physical_target =
                !join_target_outcomes_recorded;
            scanner_pg_ctx.explicit_query_ordinals = true;
			/* Ask the generic processor for its native raw evidence on every
			 * wave. CPU reports one target/source observation per pair; GPU
			 * reports its already-deduplicated logical target identity. The
			 * BatchJoin owner validates that backend-specific shape above and
			 * alone decides whether this wave publishes the physical target. */
            scanner_pg_ctx.shared_target_batch = false;
            scanner_pg_ctx.record_target_operational_outcomes = true;
			scanner_pg_ctx.cpu_prepared_target =
				join_cpu_prepared_target.unions_ready
					? &join_cpu_prepared_target : nullptr;
            for (std::size_t i = 0; i < batch_size; ++i) {
                scanner_pg_ctx.target_locators[i] = target.seq_loc;
                scanner_pg_ctx.target_rownos[i] = target.sequence_rowno;
                scanner_pg_ctx.query_ordinals[i] = join_queries[i].ordinal;
            }

            join_result_scratch = processor.process_sequences(
                batch_size,
                mmsearch_context.alphabet_code,
                mmsearch_context.storage_unit_byte_width,
                nullptr,
                &join_query_hdrs,
                join_target_hdrs,
                join_query_ids,
                base_params,
                ::bioseq::simd::HWY_NAMESPACE::BatchInvocationRole::primary,
                0,
                last_stats,
                &scanner_pg_ctx);
			normalize_shared_join_target_outcomes(
				last_stats,
				record_physical_target,
				batch_size);
			const auto prepared_after = prepared_counter_totals();
			if (prepared_after[0] < prepared_before[0]
				|| prepared_after[1] < prepared_before[1]
				|| prepared_after[2] < prepared_before[2]
				|| join_cpu_prepared_target.physical_target_index_reads
					!= prepared_after[0]) {
				ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("BioseqBatchJoin prepared-target counters are inconsistent")));
			}
			auto& operational = last_stats.operational;
			operational.cpu_prepared_index_builds +=
				prepared_after[0] - prepared_before[0];
			operational.cpu_prepared_index_reuses +=
				prepared_after[1] - prepared_before[1];
			operational.cpu_prepared_duplicate_build_attempts +=
				prepared_after[2] - prepared_before[2];
			if (join_cpu_prepared_target.unions_ready
				&& !join_cpu_prepared_target.groups.empty()
				&& !join_prepared_union_outcomes_recorded) {
				operational.cpu_prepared_unique_keys =
					join_cpu_prepared_target.total_unique_keys;
				operational.cpu_prepared_key_set_checksum =
					join_cpu_prepared_target.key_set_checksum;
				operational.cpu_prepared_key_set_checksum_present = true;
				join_prepared_union_outcomes_recorded = true;
			}
			const bool every_group_built =
				!join_cpu_prepared_target.groups.empty()
				&& std::all_of(
					join_cpu_prepared_target.groups.begin(),
					join_cpu_prepared_target.groups.end(),
					[](const auto& group) noexcept {
						return group.target_index != nullptr
							&& group.build_set_checksum_present
							&& group.build_set_checksum
								== group.key_set_checksum;
					});
			if (every_group_built
				&& !join_prepared_build_checksum_recorded) {
				operational.cpu_prepared_build_set_checksum =
					join_cpu_prepared_target.key_set_checksum;
				operational.cpu_prepared_build_set_checksum_present = true;
				join_prepared_build_checksum_recorded = true;
			}
            join_scanner_pg_cache->close_all(/*commit=*/false);
            bioseq::storage::destroy_seq_caches(join_scanner_pg_cache);
            join_scanner_pg_cache = nullptr;
            for (std::size_t i = 0; i < batch_size; ++i) {
                if (join_query_detoast_copies[i] != nullptr) {
                    pfree(join_query_detoast_copies[i]);
                    join_query_detoast_copies[i] = nullptr;
                }
            }

            join_match_map.clear();
            join_output_cursors.clear();
            for (const auto& [result_key, matches] : join_result_scratch) {
                (void)matches;
                const bool known = std::any_of(
                    join_queries.begin(), join_queries.end(),
                    [result_key](const JoinQueryRef& query) noexcept {
                        return query.ordinal
                            == static_cast<std::uint64_t>(result_key);
                    });
                if (result_key < 0 || !known) {
                    ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("BioseqBatchJoin processor returned an unknown query id"),
                         errdetail("query_id=%d", result_key)));
                }
            }
            for (std::size_t query_index = 0;
                 query_index < join_queries.size(); ++query_index) {
                const std::uint64_t ordinal = join_queries[query_index].ordinal;
                auto found = join_result_scratch.find(
                    static_cast<std::int32_t>(ordinal));
                if (found == join_result_scratch.end()) {
                    continue;
                }
                auto [inserted, did_insert] = join_match_map.emplace(
                    ordinal, std::move(found->second));
                if (!did_insert) {
                    ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("BioseqBatchJoin produced duplicate query ordinals")));
                }
                for (std::size_t match_index = 0;
                     match_index < inserted->second.size(); ++match_index) {
                    const auto& match = inserted->second[match_index];
                    if (base_params.do_cigar
                        && (!match.has_cigar || match.cigar.empty())) {
                        continue;
                    }
                    join_output_cursors.push_back(
                        JoinOutputCursor{query_index, match_index});
                }
            }
            join_output_position = 0;
            join_result_scratch.clear();
            join_target_outcomes_recorded = true;
        }
        PG_CATCH();
        {
            if (join_scanner_pg_cache != nullptr) {
                join_scanner_pg_cache->close_all(/*commit=*/false);
                bioseq::storage::destroy_seq_caches(join_scanner_pg_cache);
                join_scanner_pg_cache = nullptr;
            }
            for (std::size_t i = 0; i < batch_size; ++i) {
                if (join_query_detoast_copies[i] != nullptr) {
                    pfree(join_query_detoast_copies[i]);
                    join_query_detoast_copies[i] = nullptr;
                }
            }
            join_result_scratch.clear();
            bioseq::parallelism::pg::end_batch(ticket, false);
            PG_RE_THROW();
        }
        PG_END_TRY();
        bioseq::parallelism::pg::end_batch(ticket, true);
        return true;
    }

    void finish_join_target() {
        if (!join_mode) {
            return;
        }
		std::uint64_t builds = 0;
		std::uint64_t reuses = 0;
		std::uint64_t duplicate_build_attempts = 0;
		std::uint64_t built_groups = 0;
		for (const auto& group : join_cpu_prepared_target.groups) {
			builds += group.target_index_builds;
			reuses += group.target_index_reuses;
			duplicate_build_attempts += group.duplicate_build_attempts;
			built_groups += group.target_index != nullptr ? 1u : 0u;
		}
		if (built_groups != 0
			&& (builds != built_groups
				|| builds != join_cpu_prepared_target.physical_target_index_reads
				|| builds + reuses != join_cpu_prepared_target.query_count
				|| duplicate_build_attempts != 0
				|| !join_prepared_build_checksum_recorded)) {
			ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("BioseqBatchJoin target-wide CPU preparation equation failed"),
				 errdetail("builds=%llu built_groups=%llu reads=%llu reuses=%llu queries=%llu duplicate_build_attempts=%llu checksum_published=%s",
					static_cast<unsigned long long>(builds),
					static_cast<unsigned long long>(built_groups),
					static_cast<unsigned long long>(
						join_cpu_prepared_target.physical_target_index_reads),
					static_cast<unsigned long long>(reuses),
					static_cast<unsigned long long>(
						join_cpu_prepared_target.query_count),
					static_cast<unsigned long long>(duplicate_build_attempts),
					join_prepared_build_checksum_recorded ? "true" : "false")));
		}
        join_queries.clear();
        join_match_map.clear();
        join_output_cursors.clear();
        join_output_position = 0;
        join_target_active = false;
        join_target_prepared = false;
        join_target_outcomes_recorded = false;
		join_prepared_union_outcomes_recorded = false;
		join_prepared_build_checksum_recorded = false;
		join_cpu_prepared_target =
			::bioseq::simd::HWY_NAMESPACE::BatchProcessor::CpuPreparedTarget{};
        clear_slots();
    }

    void reset_join_runtime_for_rescan() {
        join_queries.clear();
        join_match_map.clear();
        join_output_cursors.clear();
        join_output_position = 0;
        join_target_active = false;
        join_target_prepared = false;
        join_target_outcomes_recorded = false;
		join_prepared_union_outcomes_recorded = false;
		join_prepared_build_checksum_recorded = false;
		join_cpu_prepared_target =
			::bioseq::simd::HWY_NAMESPACE::BatchProcessor::CpuPreparedTarget{};
        clear_slots();
        last_stats = ::bioseq::pipeline_stats::PipelineExecutionStats{};
    }

    /*
     * Batch-phase transition. Called when the access method returns
     * TupIsNull. Invokes BatchProcessor::process_sequences on the
     * buffered target varlenas plus the stashed query varlena; stashes
     * the returned MatchResult map; builds output_cursors in insertion
     * order (one per MatchResult per row, skipping rows whose PK is
     * absent from the map). The query/target type dispatch happens
     * inside process_sequences via pgseq_t::dispatch_instance.
     */
    [[nodiscard]] bool run_batch_and_build_cursors() {
        bioseq::parallelism::pg::backend_sync();
        if (buffered_entries.size() > kDispatchBatchCapacity) {
            ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("bioseq attempted to dispatch an oversized tuple buffer"),
                 errdetail("buffered=%zu capacity=%zu",
                    buffered_entries.size(), kDispatchBatchCapacity)));
        }
        const size_t batch_size = buffered_entries.size();
        /*
         * DOC0371 \u00a75.2: this value is one completed dispatch-buffer delta.
         * Waves and stacked extra queries below merge into it, but a later
         * ReScan/batch must never replay an earlier delta into the statement
         * collector.
         */
        last_stats = ::bioseq::pipeline_stats::PipelineExecutionStats{};
        const bool executed_nonempty_buffer =
            mmsearch_context.active && batch_size != 0;
        const std::int64_t scheduled_queries =
            static_cast<std::int64_t>(batch_size)
            * static_cast<std::int64_t>(1 + extra_query_varlenas.size());
        const auto ticket =
            bioseq::parallelism::pg::begin_batch(scheduled_queries);
        PG_TRY();
        {
            run_batch_and_build_cursors_impl();
        }
        PG_CATCH();
        {
            bioseq::parallelism::pg::end_batch(ticket, false);
            PG_RE_THROW();
        }
        PG_END_TRY();
        bioseq::parallelism::pg::end_batch(ticket, true);
        return executed_nonempty_buffer;
    }

    void run_batch_and_build_cursors_impl() {
        /*
         * DOC0294 — hydrate in row-span waves. A small batch naturally becomes
         * one wave; larger batches split at the current index-memory cap so
         * the dispatch phase does not force materialisation of rows that are
         * not active in the current wave.
         */
        std::array<const pgseq_t*, kDispatchBatchCapacity> target_hdrs{};
        std::array<const pgseq_t*, kDispatchBatchCapacity> per_row_query_hdrs{};
        std::array<int32_t,                      kDispatchBatchCapacity> query_ids{};

        const size_t batch_size = buffered_entries.size();
        if (!mmsearch_context.active || batch_size == 0) {
            match_map.clear();
            output_cursors.clear();
            output_position = 0;
            return;
        }

        /*
         * We use the buffer entry index `i` as the BatchProcessor's
         * key rather than the row's primary_key column. The PK from
         * the user's table is only available when the targetlist
         * includes a bioseq.<projection>(pk) FuncExpr (which is what
         * BeginCustomScan walks to discover `primary_key_attno`).
         * For queries without such a projection — e.g. plain
         * `SELECT t.pk FROM t WHERE mmsearch(t.seq, ...)` or
         * cross-table mmsearch joins where the qual is the only
         * bioseq surface — primary_key_attno stays
         * InvalidAttrNumber and every row would map to query_id=-1,
         * letting the std::map key collapse all matches into one
         * bucket. Using the entry index as the key keeps each row
         * uniquely identifiable inside the batch regardless of
         * targetlist shape.
         */
        std::vector<size_t> all_rows(batch_size);
        std::iota(all_rows.begin(), all_rows.end(), size_t{0});

        std::size_t lpu_for_hydration = 0;
        std::size_t bytes_per_unit_for_hydration = 0;
        pgseq_t::dispatch_instance(
            mmsearch_context.alphabet_code,
            mmsearch_context.storage_unit_byte_width,
            [&lpu_for_hydration, &bytes_per_unit_for_hydration]<class S>() noexcept {
                using traits = typename compact_sequence_range<S>::traits;
                lpu_for_hydration = traits::letters_per_unit;
                bytes_per_unit_for_hydration =
                    sizeof(typename traits::storage_unit);
            });
        auto estimate_locator_hydration_bytes =
            [lpu_for_hydration,
             bytes_per_unit_for_hydration](const bioseq::storage::seq_locator& loc)
                -> std::uint64_t {
            if (lpu_for_hydration == 0 || bytes_per_unit_for_hydration == 0) {
                return 0;
            }
            const std::uint64_t total_units =
                (static_cast<std::uint64_t>(loc.total_bp_size)
                    + lpu_for_hydration - 1)
                / lpu_for_hydration;
            if (total_units
                > (std::numeric_limits<std::uint64_t>::max)()
                    / static_cast<std::uint64_t>(bytes_per_unit_for_hydration)) {
                return 0;
            }
            const std::uint64_t payload_bytes =
                total_units
                * static_cast<std::uint64_t>(bytes_per_unit_for_hydration);
            if (payload_bytes
                > (std::numeric_limits<std::uint64_t>::max)()
                    - static_cast<std::uint64_t>(pgseq_t::header_bytes())) {
                return 0;
            }
            const std::uint64_t varlena_bytes =
                static_cast<std::uint64_t>(pgseq_t::header_bytes())
                + payload_bytes;
            if (varlena_bytes > static_cast<std::uint64_t>(MaxAllocSize)) {
                return 0;
            }
            return varlena_bytes;
        };
        auto estimate_hydration_bytes =
            [this, &estimate_locator_hydration_bytes](size_t row) -> std::uint64_t {
            if (row >= buffered_entries.size()) {
                return 0;
            }
            const BufferedScanEntry& entry = buffered_entries[row];
            std::uint64_t bytes = 0;
            if (entry.seq_loc.has_value()) {
                bytes += estimate_locator_hydration_bytes(*entry.seq_loc);
            }
            if (entry.per_row_query_loc.has_value()) {
                const std::uint64_t query_bytes =
                    estimate_locator_hydration_bytes(*entry.per_row_query_loc);
                if (query_bytes
                    > (std::numeric_limits<std::uint64_t>::max)() - bytes) {
                    return (std::numeric_limits<std::uint64_t>::max)();
                }
                bytes += query_bytes;
            }
            return bytes;
        };
        auto refresh_row_arrays =
            [this, &target_hdrs, &per_row_query_hdrs, &query_ids](std::span<const size_t> rows) {
            for (const size_t i : rows) {
                if (i >= buffered_entries.size() || i >= kDispatchBatchCapacity) {
                    continue;
                }
                const BufferedScanEntry& entry = buffered_entries[i];
                target_hdrs[i] = entry.seq_hdr();  // nullptr for NULL seq
                per_row_query_hdrs[i] = entry.per_row_query_hdr();
                query_ids[i] = static_cast<int32_t>(i);
            }
        };

        std::vector<std::pair<size_t, size_t>> waves;
        const std::uint64_t budget =
            bioseq::parallelism::MemoryAdmissionController::instance()
                .allocatable_after_reservations();
        std::uint64_t wave_bytes = 0;
        size_t wave_begin = 0;
        for (size_t i = 0; i < batch_size; ++i) {
            const std::uint64_t row_bytes = estimate_hydration_bytes(i);
            const bool split =
                budget != (std::numeric_limits<std::uint64_t>::max)()
                && wave_bytes != 0
                && row_bytes > 0
                && row_bytes > budget - std::min(wave_bytes, budget);
            if (split) {
                waves.emplace_back(wave_begin, i);
                wave_begin = i;
                wave_bytes = 0;
            }
            if (row_bytes > (std::numeric_limits<std::uint64_t>::max)() - wave_bytes) {
                wave_bytes = (std::numeric_limits<std::uint64_t>::max)();
            } else {
                wave_bytes += row_bytes;
            }
        }
        if (batch_size > 0) {
            waves.emplace_back(wave_begin, batch_size);
        }

        const pgseq_t* query_hdr = mmsearch_context.query_hdr();
        const uint32_t query_bp_size = query_hdr != nullptr ? query_hdr->bp_size : 0;
        bioseq_log<DBG>("[bioseq] pg: TupleDispatchBuffer calling BatchProcessor::process_sequences batch_size={} typmod=({},{}) query_bp_size={} read_index={} per_row_mode={} alphabet_recompute={}",
                batch_size,
                static_cast<unsigned>(mmsearch_context.alphabet_code),
                static_cast<unsigned>(mmsearch_context.storage_unit_byte_width),
                query_bp_size,
                read_sequence_index_active,
                per_row_mode(),
                mmsearch_context.recompute_due_to_alphabet_mismatch);

        // DOC0264 §9.2 / DOC0274 — wire scanner_pg into the dispatch loop.
        // Allocates a fresh seq_caches for scanner_pg's PG-backed reads.
        // Lives in batch_payload_ctx so palloc'd state survives the call.
        if (batch_payload_ctx == nullptr) {
            batch_payload_ctx = AllocSetContextCreate(CurrentMemoryContext,
                "TupleDispatchBuffer payload arena", ALLOCSET_DEFAULT_SIZES);
        }
        bioseq::storage::seq_caches* scanner_pg_cache =
            bioseq::storage::alloc_seq_caches(batch_payload_ctx);
        scanner_pg_cache->side                = bioseq::storage::seq_caches::Side::Read;
        scanner_pg_cache->lock_mode           = AccessShareLock;
        scanner_pg_cache->read_state_mctx     = batch_payload_ctx;
        scanner_pg_cache->main_database_oid   = user_main_database_oid;
        scanner_pg_cache->main_tablespace_oid = user_main_tablespace_oid;
        scanner_pg_cache->main_relfilenode    = user_main_relfilenode;

        typename ::bioseq::simd::HWY_NAMESPACE::BatchProcessor::ScannerPgContext scanner_pg_ctx{};
        scanner_pg_ctx.db_oid             = user_main_database_oid;
        scanner_pg_ctx.tablespace_oid     = user_main_tablespace_oid;
        scanner_pg_ctx.main_relfilenode   = user_main_relfilenode;
        scanner_pg_ctx.mmsearch_seq_attno =
            static_cast<std::int16_t>(mmsearch_context.seq_attno);
        scanner_pg_ctx.caches             = scanner_pg_cache;
        scanner_pg_ctx.parent_mctx        = batch_payload_ctx;
        scanner_pg_ctx.snapshot           = scan_snapshot;
        // Populate per-row locators for the target column. The production
        // scanner_pg path uses these. Last-fragment padding is recomputed
        // inside process_sequences where the template parameter S (and thus
        // its letters_per_unit) is available.
        for (size_t i = 0; i < batch_size; ++i) {
            const auto& seq_loc_opt = buffered_entries[i].seq_loc;
            if (seq_loc_opt.has_value()) {
                scanner_pg_ctx.target_locators[i] = *seq_loc_opt;
            }
            // DOC0279 §11.3c — surface the per-row columnar rowno (the index
            // query_key) so the stored per-fragment index can be assembled.
            scanner_pg_ctx.target_rownos[i] = buffered_entries[i].sequence_rowno;
        }
        scanner_pg_ctx.user_relid        = user_relid;
        scanner_pg_ctx.index_read_requirement = index_read_requirement;
        scanner_pg_ctx.read_index_active = read_sequence_index_active
            && !mmsearch_context.recompute_due_to_alphabet_mismatch;
        scanner_pg_ctx.index_async_persist_active = bioseq_index_async_persist;
        scanner_pg_ctx.recompute_async_persist_active =
            bioseq_recompute_async_persist;
        // DOC0279 §11.3/§12.3 — resolve the target column's active minimizer
        // policy ONCE per batch (catalog lookup; default
        // minimizers_per_fragment). Stored-index reads use it when
        // read_index_active is true; no-index recompute also uses it to select
        // the transient MPF/MPS index shape without reading stored chunks.
        scanner_pg_ctx.active_index =
            std::string{bioseq::indexing::DEFAULT_ACTIVE_READ_INDEX};
        // The active-policy catalog row selects the transient MPF/MPS shape
        // even when payload reads are forbidden. Resolving policy metadata is
        // not a persisted target-index payload probe and must not mutate the
        // existing no-index algorithm contract.
        if (OidIsValid(user_relid)) {
            scanner_pg_ctx.active_index = bioseq::storage::indexing::resolve_active_read_index(
                user_relid, user_main_relfilenode,
                static_cast<std::int16_t>(mmsearch_context.seq_attno));
        }
        auto reset_dispatch_arrays =
            [&target_hdrs, &per_row_query_hdrs, &query_ids] {
            target_hdrs.fill(nullptr);
            per_row_query_hdrs.fill(nullptr);
            query_ids.fill(-1);
        };
        auto reset_scanner_wave =
            [&scanner_pg_ctx] {
            scanner_pg_ctx.target_locators.fill(std::nullopt);
            scanner_pg_ctx.target_rownos.fill(-1);
            scanner_pg_ctx.target_index_sources.fill(
                ::bioseq::pipeline_stats::TargetIndexSource::none);
        };
        auto install_scanner_wave =
            [this, &scanner_pg_ctx](std::span<const size_t> rows) {
            for (const size_t row : rows) {
                if (row >= buffered_entries.size() || row >= kDispatchBatchCapacity) {
                    continue;
                }
                const auto& seq_loc_opt = buffered_entries[row].seq_loc;
                if (seq_loc_opt.has_value()) {
                    scanner_pg_ctx.target_locators[row] = *seq_loc_opt;
                }
                scanner_pg_ctx.target_rownos[row] = buffered_entries[row].sequence_rowno;
            }
        };
        auto wave_estimated_bytes =
            [&estimate_hydration_bytes](std::span<const size_t> rows) -> std::uint64_t {
            std::uint64_t bytes = 0;
            for (const size_t row : rows) {
                const std::uint64_t row_bytes = estimate_hydration_bytes(row);
                if (row_bytes > (std::numeric_limits<std::uint64_t>::max)() - bytes) {
                    return (std::numeric_limits<std::uint64_t>::max)();
                }
                bytes += row_bytes;
            }
            return bytes;
        };
        auto merge_stats =
            [this](const ::bioseq::pipeline_stats::PipelineExecutionStats& wave_stats) {
            last_stats.merge(wave_stats);
        };

        match_map.clear();
        // Pointer to the context.
        auto* scanner_pg_arg = &scanner_pg_ctx;
        active_wave_permit.reset();

        PG_TRY();
        {
            for (const auto& wave : waves) {
                const std::span<const size_t> rows{
                    all_rows.data() + wave.first, wave.second - wave.first};
                const std::uint64_t estimated_wave_bytes = wave_estimated_bytes(rows);
                active_wave_permit =
                    bioseq::parallelism::MemoryAdmissionController::instance()
                        .try_reserve(estimated_wave_bytes);
                const bool hydrate_wave =
                    estimated_wave_bytes == 0
                    || active_wave_permit.has_value();
                if (!hydrate_wave) {
                    bioseq_log<DBG>("[bioseq] pg: TupleDispatchBuffer wave [{}, {}) was not admitted for {} bytes; target locators stream through scanner_pg",
                            wave.first, wave.second,
                            static_cast<unsigned long long>(estimated_wave_bytes));
                }
                reset_dispatch_arrays();
                reset_scanner_wave();
                if (hydrate_wave) {
                    hydrate_locators_by_bucket(rows);
                }
                refresh_row_arrays(rows);
                install_scanner_wave(rows);

                ::bioseq::pipeline_stats::PipelineExecutionStats wave_stats{};
                std::map<int32_t, std::vector<::bioseq::simd::HWY_NAMESPACE::MatchResult>> wave_match_map;
                if (per_row_mode()) {
                    wave_match_map = processor.process_sequences(
                        batch_size,
                        mmsearch_context.alphabet_code,
                        mmsearch_context.storage_unit_byte_width,
                        nullptr,
                        &per_row_query_hdrs,
                        target_hdrs, query_ids,
                        base_params,
                        ::bioseq::simd::HWY_NAMESPACE::BatchInvocationRole::primary,
                        0,
                        wave_stats,
                        scanner_pg_arg);
                } else {
                    wave_match_map = processor.process_sequences(
                        batch_size,
                        mmsearch_context.alphabet_code,
                        mmsearch_context.storage_unit_byte_width,
                        query_hdr,
                        nullptr,
                        target_hdrs, query_ids,
                        base_params,
                        ::bioseq::simd::HWY_NAMESPACE::BatchInvocationRole::primary,
                        0,
                        wave_stats,
                        scanner_pg_arg);
                }
                merge_stats(wave_stats);

                /*
                 * For every extra mmsearch qual the caller registered we run
                 * an independent process_sequences pass over the same active
                 * wave, then logically AND the matches into the wave map.
                 */
                for (size_t extra_idx = 0;
                     extra_idx < extra_query_varlenas.size(); ++extra_idx) {
                    const std::vector<char>& extra_bytes =
                        extra_query_varlenas[extra_idx];
                    if (extra_bytes.empty()) {
                        continue;
                    }
                    const pgseq_t* extra_hdr =
                        reinterpret_cast<const pgseq_t*>(extra_bytes.data());
                    ::bioseq::pipeline_stats::PipelineExecutionStats extra_stats{};
                    auto extra_match_map =
                        processor.process_sequences(
                            batch_size,
                            mmsearch_context.alphabet_code,
                            mmsearch_context.storage_unit_byte_width,
                            extra_hdr,
                            nullptr,
                            target_hdrs, query_ids,
                            base_params,
                            ::bioseq::simd::HWY_NAMESPACE::BatchInvocationRole::extra_query,
                            extra_broadcast_query_ordinal(
                                per_row_mode(), batch_size, extra_idx),
                            extra_stats,
                            scanner_pg_arg);
                    merge_stats(extra_stats);
                    bioseq_log<DBG>("[bioseq] pg: TupleDispatchBuffer extra-query #{} wave [{}, {}) produced matches for {} entries",
                            extra_idx, wave.first, wave.second,
                            extra_match_map.size());
                    for (auto it = wave_match_map.begin();
                         it != wave_match_map.end();) {
                        if (extra_match_map.find(it->first)
                            == extra_match_map.end()) {
                            it = wave_match_map.erase(it);
                        } else {
                            ++it;
                        }
                    }
                }

                for (auto& [entry_id, matches] : wave_match_map) {
                    match_map[entry_id] = std::move(matches);
                }
                active_wave_permit.reset();
            }

            // DOC0274 — release scanner_pg's per-batch seq_caches now that all
            // process_sequences calls have returned. close_all releases the
            // sub-table relations and their BioseqColumnReadStates;
            // destroy_seq_caches returns the slab to the parent context.
            scanner_pg_cache->close_all(/*commit=*/false);
            bioseq::storage::destroy_seq_caches(scanner_pg_cache);
            scanner_pg_cache = nullptr;
        }
        PG_CATCH();
        {
            // PostgreSQL ERROR is a longjmp, so C++ destructors cannot own
            // relation/snapshot cleanup.  Release scanner_pg resources before
            // preserving and rethrowing the original server error.
            active_wave_permit.reset();
            if (scanner_pg_cache != nullptr) {
                scanner_pg_cache->close_all(/*commit=*/false);
                bioseq::storage::destroy_seq_caches(scanner_pg_cache);
                scanner_pg_cache = nullptr;
            }
            PG_RE_THROW();
        }
        PG_END_TRY();

        output_cursors.clear();
        for (size_t i = 0; i < buffered_entries.size(); ++i) {
            const BufferedScanEntry& entry = buffered_entries[i];
            /*
             * Match map is keyed on the entry index (see comment in
             * the loop above that built `query_ids[i] = i`). Skip
             * entries whose index is not in the map — those rows had
             * no usable target sequence (NULL seq column) or the
             * aligner produced zero matches; either way `WHERE
             * mmsearch(...)` evaluates to FALSE for them.
             */
            auto hit = match_map.find(static_cast<int32_t>(i));
            if (hit == match_map.end()) {
                bioseq_log<DBG>("[bioseq] pg: batch miss for entry index={}; WHERE mmsearch evaluates to FALSE",
                        i);
                continue;
            }
            /*
             * Filter degenerate matches. The aligner can return a
             * MatchResult that has a chain pinned in the seed pass
             * but no usable base alignment (`has_cigar == false` or
             * `cigar` empty). When the user requested a CIGAR via
             * `bioseq.cigar(...)`, an empty/no-cigar match is the
             * fuzzy-search equivalent of "no real hit": surfacing it
             * as a row would mean WHERE mmsearch(...) acts more like
             * a tautology than a filter, which contradicts the
             * substring-match semantics users expect of mmsearch.
             *
             * Skipping these here filters them out at output-cursor
             * build time; the row never reaches the projection
             * pipeline, so its bioseq.cigar / bioseq.target_start /
             * etc. would have come back as the placeholder Const's
             * default-NULL state — exactly the kind of "row but
             * empty fields" output the user reported as undesired.
             *
             * When `do_cigar` is false (the user only projects e.g.
             * bioseq.target_start / bioseq.score), we don't have a
             * cigar to inspect; in that case we keep every match
             * the chainer surfaced because the chain itself is
             * meaningful (passed `min_seed_count` / `min_chain_score`).
             */
            for (size_t m = 0; m < hit->second.size(); ++m) {
                const ::bioseq::simd::HWY_NAMESPACE::MatchResult& match_candidate = hit->second[m];
                if (base_params.do_cigar
                    && (!match_candidate.has_cigar
                        || match_candidate.cigar.empty())) {
                    bioseq_log<DBG>("[bioseq] pg: batch skip: degenerate match for entry index={} cursor m={} (do_cigar=true but cigar empty)",
                            i, m);
                    continue;
                }
                output_cursors.push_back({ i, m });
            }
        }
        output_position = 0;
        bioseq_log<DBG>("[bioseq] pg: batch produced {} output tuples from {} scan tuples",
                output_cursors.size(), buffered_entries.size());
    }

    /*
     * Reading-phase iterator. nullopt means only genuine cursor exhaustion.
     * A cursor whose entry or match no longer exists is internal corruption,
     * not EOF, and therefore raises ERROR fail closed.
     */
    [[nodiscard]] std::optional<NextOutput> read_next_output() {
        if (join_mode) {
            const CursorBoundary boundary = classify_output_cursor(
                join_output_position, join_output_cursors.size());
            if (boundary == CursorBoundary::exact_eof) {
                return std::nullopt;
            }
            if (boundary == CursorBoundary::corrupt) {
                ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("BioseqBatchJoin output cursor position is corrupt"),
                     errdetail("position=%zu size=%zu",
                        join_output_position, join_output_cursors.size())));
            }
            const JoinOutputCursor cursor =
                join_output_cursors[join_output_position++];
            if (buffered_entries.size() != 1
                || cursor.query_index >= join_queries.size()) {
                ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("BioseqBatchJoin output cursor references missing input")));
            }
            const std::uint64_t ordinal =
                join_queries[cursor.query_index].ordinal;
            auto found = join_match_map.find(ordinal);
            if (found == join_match_map.end()
                || cursor.match_index >= found->second.size()) {
                ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("BioseqBatchJoin output cursor references a missing match")));
            }
            return NextOutput{
                &buffered_entries.front(),
                ordinal,
                &found->second[cursor.match_index],
            };
        }
        const CursorBoundary boundary = classify_output_cursor(
            static_cast<std::int64_t>(output_position), output_cursors.size());
        if (boundary == CursorBoundary::exact_eof) {
            return std::nullopt;
        }
        if (boundary == CursorBoundary::corrupt) {
            ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq output cursor position is corrupt"),
                 errdetail("position=%d size=%zu",
                    output_position, output_cursors.size())));
        }
        const OutputCursor& cursor = output_cursors[output_position];
        output_position++;
        if (cursor.scan_entry_index >= buffered_entries.size()) {
            ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq output cursor references a missing scan entry"),
                 errdetail("entry=%zu buffered=%zu",
                    cursor.scan_entry_index, buffered_entries.size())));
        }
        BufferedScanEntry& entry = buffered_entries[cursor.scan_entry_index];
        auto hit = match_map.find(static_cast<int32_t>(cursor.scan_entry_index));
        if (hit == match_map.end() || cursor.match_index >= hit->second.size()) {
            ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq output cursor references a missing alignment match"),
                 errdetail("entry=%zu match=%zu available=%zu",
                    cursor.scan_entry_index,
                    cursor.match_index,
                    hit == match_map.end() ? std::size_t{0} : hit->second.size())));
        }
        return NextOutput{ &entry, 0, &hit->second[cursor.match_index] };
    }

    /*
     * True once run_batch_and_build_cursors has been called for the
     * current scan. Inner_CustomExecScan uses this to switch from the
     * writing-phase access-method loop to the reading-phase drain loop.
     */
    [[nodiscard]] constexpr bool has_finished_writing() const {
        return output_position >= 0;
    }

    /*
     * True when BeginCustomScan found a WHERE mmsearch(...) qual and
     * installed its (seq_attno, query_varlena, typmod) context. When
     * false the custom scan must behave like a pass-through scan:
     * fetch rows, project them directly via the (VarInjectorMutator-
     * rewritten) target list, and return them without buffering. The
     * buffering + batch pipeline is only meaningful when there is a
     * WHERE mmsearch(...) to drive it. See SPEC 2 §6.8 for the
     * historical gap this accessor closes.
     */
    [[nodiscard]] constexpr bool mmsearch_active() const {
        return mmsearch_context.active;
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
     * mmsearch_context untouched so callers can reuse configuration.
     */
    [[gnu::always_inline]] inline void clear_slots() {
        bioseq_log<DBG>("[bioseq] pg: TupleDispatchBuffer::clear_slots() entered; freeing {} slots, {} outputs",
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
        /*
         * DOC0260 §9.5 — the bulk-fetched payloads (and the per-row query
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

} // namespace bioseq::scheduler
