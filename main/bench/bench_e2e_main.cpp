/**
 * @file bench_e2e_main.cpp
 * @brief End-to-end SQL-driven benchmark (DOC0286 Part 2). bioseq via
 *        bioseq.load + SELECT mmsearch through libpq; minimap2 via CLI on an
 *        INT32-safe split FASTA. 2/4 Gbp x dna4/dna5 x {no_index, just_index} x 3 reps. The
 *        measured region is "e2e_measured"; setup/index-build lives in on_input.
 */
#include "common_simd.h"                   // HWY_BEFORE/AFTER_NAMESPACE + HWY_NAMESPACE
#include "common.h"
#include <simdjson.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <queue>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <type_traits>
#include <vector>

import bioseq.common.mlogging;
import bench_framework;
import bioseq.common.templating;           // CompTimeStr for alphabet_entry
import bioseq.instr;                       // region(), register_region_static
import bioseq.integration_tests.common;           // connection_test_context, exec_sqlf, query_rows
import bioseq.common.sequan_compat;                      // seq::dna4 / seq::dna5
import bioseq.sequences;                   // compact_sequence_range, traits, supported_alphabets, to_string()
import bioseq.sequences.generation;        // generator_params / alphabet_sequence_generator<A,U>
import bioseq.io.writer;                    // sequence_writer<A, FASTA>
import bioseq.io.public_api;                // compact_sequence_view<R>
import bioseq.io.options;                   // sequence_io_file_type, writer_options
import bioseq.indexing.type;                 // canonical minimizer index names
import bioseq.indexing.minimizer_hashmap;    // exact-SIMD sketch proof + rev3 sizing constants
import bioseq.parallelism.memory_admission;  // bounded prototype-owned routing state
import bioseq.sequences.kmers;               // sketch_collector / MinimapSketchData
import bioseq.sequences.chaining.utils;      // Anchor exact-final-byte equation
import bioseq.sequences.pipe;                // canonical execution-stage matrix

#ifndef BIOSEQ_E2E_REPORT_DIR
#error "BIOSEQ_E2E_REPORT_DIR must be defined by the build system"
#endif
#ifndef BIOSEQ_E2E_DATA_DIR
#error "BIOSEQ_E2E_DATA_DIR must be defined by the build system"
#endif
#ifndef BIOSEQ_E2E_CONFIG_PATH
#error "BIOSEQ_E2E_CONFIG_PATH must be defined by the build system"
#endif
#ifndef BIOSEQ_E2E_MINIMAP2_BIN
#define BIOSEQ_E2E_MINIMAP2_BIN "minimap2"     // overridable via -D; PATH fallback
#endif

HWY_BEFORE_NAMESPACE();   // open the Highway per-target namespace
namespace {
using bioseq::bench_framework::ParamMap;
using bioseq::bench_framework::RunConstraints;
using bioseq::bench_framework::create_benchmark;
namespace gen = bioseq::sequences::generation;
// The E2E bench fixes only the storage width. Alphabet comes from the JSON
// test-case data (`dna4` or `dna5`) and is dispatched at compile time. uint64
// keeps generated ranges byte-identical to `bioseq.seq('<alphabet>', 64)` and to
// the IO writer's view, while the generic generator default remains uint32.
using bench_storage_t  = std::uint64_t;
template <sequence_alphabet A>
using bench_config = sequence_custom_alphabet<A, bench_storage_t>;
template <sequence_alphabet A>
using bench_sequence_range = compact_sequence_range<bench_config<A>>;
template <sequence_alphabet A>
using bench_pair_t =
    typename gen::alphabet_sequence_generator<A, bench_storage_t>::query_target_pair;

template <sequence_alphabet A>
[[nodiscard]] constexpr std::string_view bench_alphabet_name() {
    if constexpr (std::same_as<A, seq::dna4>) return "dna4";
    else if constexpr (std::same_as<A, seq::dna5>) return "dna5";
    else return "?";
}
template <sequence_alphabet A>
[[nodiscard]] inline std::string bench_seq_type() {
    return fmt::format("bioseq.seq('{}', 64)", bench_alphabet_name<A>());
}
namespace it = bioseq_int_tests;

// The single measured region (registered once at static init).
const auto* const kMeasuredRegion =
    bioseq::instr::register_region_static("e2e_measured");
const auto* const kRoutingPocMeasuredRegion =
    bioseq::instr::register_region_static("e2e_routing_poc_measured");

// ---- named constants (no scattered magic literals — MEDIUM-05) ----
// minimap2 stores each parsed sequence length in a SIGNED int and asserts
// `l_seq <= INT32_MAX` (bench/tool_minimap2/minimap2/bseq.c:97, bseq.h:15). The
// real per-record ceiling is therefore 2^31-1, NOT the mm_idx_seq_t::len uint32
// 2^32 the earlier draft claimed (HIGH-08).
constexpr std::int64_t  kMinimap2MaxRecordBp = 2'147'483'647;            // INT32_MAX
constexpr std::int64_t  kMinimap2SplitBp     = 2'000'000'352;            // < ceiling
static_assert(kMinimap2SplitBp <= kMinimap2MaxRecordBp,
              "minimap2 split records must not exceed INT32_MAX bases");
using dna4_u64_traits = compact_sequence_range<
    sequence_custom_alphabet<seq::dna4, bench_storage_t>>::traits;
using dna5_u64_traits = compact_sequence_range<
    sequence_custom_alphabet<seq::dna5, bench_storage_t>>::traits;
constexpr std::size_t kBenchSplitAlignmentBp =
    std::lcm(dna4_u64_traits::letters_per_unit, dna5_u64_traits::letters_per_unit);
static_assert(kMinimap2SplitBp % static_cast<std::int64_t>(kBenchSplitAlignmentBp) == 0,
              "split must be unit-aligned for every benchmark alphabet");
constexpr std::int64_t  kDefaultReferenceLengthBp        = 2'000'000'000;
constexpr std::int64_t  kPerSeqIndexPreflightThresholdBp = 4'000'000'000LL;
constexpr int           kDefaultKmerSize    = 15;
constexpr int           kDefaultWindowSize  = 10;
// Similar mappings are accepted only when their coordinate delta is strictly
// below 15 bp. A 15 bp delta itself is a failure.
constexpr std::int64_t  kPositionToleranceBp = 15;
constexpr double        kNoMatchOffsetSentinel = std::numeric_limits<double>::infinity();
constexpr std::size_t   kPopenBufBytes      = std::size_t{1} << 16;
constexpr std::size_t   kFastaStreamChunkBp = std::size_t{1} << 24;      // bounded writer body slice
// DOC0286 — FASTA bodies are line-WRAPPED so bioseq.load's reader keeps a bounded
// input slab for EVERY alphabet/width: the per-line FSM fallback (dna5, and the
// non-uint64 dna4 widths) buffers a whole body LINE before encoding, so a single
// >MaxAllocSize line would force an unbounded slab. 64 KiB lines are far under the
// reader's 1 MiB slab and are filtered by the SIMD encoders' newline LUT.
constexpr std::size_t   kFastaLineWidth     = std::size_t{1} << 16;
constexpr std::string_view kMinimizersPerFragmentIndex =
    bioseq::indexing::MINIMIZERS_PER_FRAGMENT_INDEX_NAME;
constexpr std::string_view kMinimizersPerSequenceIndex =
    bioseq::indexing::MINIMIZERS_PER_SEQUENCE_INDEX_NAME;

enum class JustIndexSetupKind {
    Scalar,
    Simd,
    Gpu,
};

struct E2eSetupSelection {
    bool scalar_just_index = false;
    bool simd_just_index = false;
    bool gpu_just_index = false;
    bool minimap2_just_index = false;
};

struct E2eJustIndexSetupRequirements {
    bool scalar_per_fragment = false;
    bool scalar_per_sequence = false;
    bool simd_per_fragment = false;
    bool simd_per_sequence = false;
    bool gpu_per_fragment = false;
    bool gpu_per_sequence = false;
    bool minimap2 = false;

    [[nodiscard]] bool any_bioseq() const noexcept {
        return scalar_per_fragment || scalar_per_sequence
            || simd_per_fragment || simd_per_sequence
            || gpu_per_fragment || gpu_per_sequence;
    }
};

struct E2eQueryTruth {
    std::int32_t id = 0;
    std::string bases;
    std::int64_t truth_target_start = 0;
    std::int64_t truth_target_end = 0;
    bool truth_is_reverse = false;
    std::int64_t target_window_start = 0;
    std::string target_window_bases;
};

struct E2eInput {
    // Connection is shared (const-usable; SQL mutates the DB, not this struct).
    std::shared_ptr<it::connection_test_context> conn;
    std::string alphabet;                        // "dna4" / "dna5"
    std::string seq_type;                        // bioseq.seq('<alphabet>', 64)
    std::string size_label;                      // "2gbp" / "4gbp"
    std::string just_index_table;                // legacy alias for the per-sequence table
    std::string just_index_table_per_fragment;   // loaded + per-fragment indexed in on_input
    std::string just_index_table_per_sequence;   // loaded + per-sequence indexed in on_input
    std::string just_index_table_simd_per_fragment;
    std::string just_index_table_simd_per_sequence;
    std::string just_index_table_scalar_per_fragment;
    std::string just_index_table_scalar_per_sequence;
    std::string just_index_table_gpu_per_fragment;
    std::string just_index_table_gpu_per_sequence;
    // The BIOSEQ table AM owns this identity.  It is not a zero-based ordinal:
    // capture it from the loaded row's ctid and carry that exact query key into
    // every READY-generation inspection before and after setup.
    std::map<std::string, std::int64_t> just_index_sequence_rowno_by_table;
    std::string no_index_table;                  // base target loaded untimed; no minimizer sidecar
    std::string target_fasta;                    // single-record, for bioseq.load
    std::string target_fasta_split;              // <=split_bp records, for minimap2
    std::string query_fasta;                     // for minimap2
    std::string mmi_path;                        // prebuilt minimap2 index
    std::string query_bases;                     // ACGTN text for the bioseq SQL literal
    std::vector<E2eQueryTruth> queries;
    int kmer_size = 15, window_size = 10;
    bool cigar_projection = false;
    bool designed_indel_cigar = false;
    bool designed_many_gap_cigar = false;
    bool require_cigar_indel = false;
    std::uint32_t min_cigar_deletion_ops = 0;
    std::uint32_t min_cigar_insertion_ops = 0;
    std::size_t minimap2_split_bp =
        static_cast<std::size_t>(kMinimap2SplitBp);  // PAF absolute-coord remap (§11)
    std::int64_t truth_target_start = 0;
    bool truth_is_reverse = false;
    std::int64_t target_bp = 0;
};

struct ColdNoIndexPolicyAbsenceEvidence {
    std::int64_t target_rows = -1;
    std::int64_t target_cardinality_metadata_only = 0;
    std::int64_t source_parent_rows_read = -1;
    std::int64_t source_fragment_rows_read = -1;
    std::int64_t source_decoded_bp = -1;
    std::int64_t physical_index_relations = -1;
    std::int64_t persisted_index_rows = -1;
    std::int64_t generation_objects = -1;
};

struct E2eOutput {
	static constexpr std::size_t kAlignmentSnapshotV1ColumnCount = 150;
	static constexpr std::size_t kAlignmentSnapshotColumnCount = 151;
    static constexpr std::size_t kAlignmentSnapshotRowCount = 20;
    bool found = false;
    std::int64_t target_start = -1;
    bool is_reverse = false;
    std::int64_t match_count = 0;
    std::int64_t expected_match_count = 0;
    std::int64_t correct_match_count = 0;
    std::int64_t exact_primary_count_accepted = 0;
    std::int64_t strand_result_count = 0;
    std::int64_t correct_strand_count = 0;
    std::int64_t cigar_result_count = 0;
    std::int64_t correct_cigar_count = 0;
    std::int64_t cigar_replay_count = 0;
    double abs_offset_bp = 0.0;
    double max_abs_offset_bp = 0.0;
    std::string requested_backend;
    std::string actual_backend;
    std::string requested_target_index_source;
    std::string actual_target_index_source;
    std::string requested_simd_chaining_policy;
    std::string observed_simd_chaining_policy;
    std::string requested_simd_alignment_algorithm;
    std::string observed_simd_alignment_algorithm;
    std::int64_t simd_evidence_schema_valid = -1;
    std::int64_t nthash_vector_batches = -1;
    std::int64_t nthash_resident_vector_stream_kernel_calls = -1;
    std::int64_t simd_chaining_exact_policy_calls = -1;
    std::int64_t simd_chaining_bounded_policy_calls = -1;
    std::int64_t simd_chaining_bounded_score_batches = -1;
    std::int64_t simd_chaining_bounded_cutoff_stops = -1;
    std::int64_t simd_chaining_bounded_candidates_skipped = -1;
    std::int64_t simd_chaining_policy_rejections = -1;
    std::int64_t simd_chaining_policy_evidence_saturated = -1;
    std::int64_t block_aligner_execution_proof_present = -1;
    std::int64_t block_aligner_execution_proof_accepted = -1;
    std::int64_t block_aligner_executed_calls = -1;
    std::int64_t block_aligner_nonempty_calls = -1;
    std::int64_t block_aligner_nonempty_calls_with_simd = -1;
    std::int64_t block_aligner_simd_dp_vector_batches = -1;
    std::int64_t block_aligner_simd_dp_cells = -1;
    std::int64_t block_aligner_forbidden_ksw2_calls = -1;
    std::int64_t block_aligner_forbidden_scalar_biological_fallbacks = -1;
    std::int64_t block_aligner_execution_proof_saturated = -1;
    std::int64_t pipeline_proof_present = 0;
    std::int64_t pipeline_proof_attached_to_measurement = 0;
    std::int64_t pipeline_proof_accepted = 0;
    std::int64_t stage_engine_matrix_valid = -1;
    std::int64_t prior_alignment_execution_id = -1;
    std::int64_t alignment_execution_id = -1;
    std::int64_t alignment_backend_pid = -1;
    std::int64_t alignment_database_oid = -1;
    std::string timed_postmaster_start_time;
    std::int64_t effective_cpu_workers = -1;
    std::int64_t comparison_cpu_workers = -1;
    std::string gpu_device_uuid;
    std::string gpu_driver;
    std::string gpu_pipeline_cache_policy;
    std::string gpu_pipeline_cache_seed_sha256;
    std::int64_t gpu_pipeline_cache_seed_size = -1;
    std::string gpu_pipeline_cache_manifest_checksum;
    std::int64_t gpu_pipeline_cache_hit = -1;
    std::int64_t alignment_scan_node_count = -1;
    std::int64_t nested_alignment_executor_count = -1;
    std::int64_t alignment_completed = -1;
    std::int64_t alignment_partial = -1;
    std::int64_t alignment_fully_drained = -1;
    std::int64_t persisted_generation_id = -1;
    std::string persisted_source_identity;
    std::int64_t attempted_alignment_jobs = -1;
    std::int64_t fully_attested_alignment_jobs = -1;
    std::int64_t unattested_alignment_jobs = -1;
    std::int64_t first_unattested_target_row = -1;
    std::int64_t first_unattested_query_ordinal = -1;
    std::int64_t cpu_prepared_index_builds = -1;
    std::int64_t cpu_prepared_index_reuses = -1;
    std::int64_t cpu_prepared_unique_keys = -1;
    std::int64_t cpu_prepared_duplicate_build_attempts = -1;
    std::string cpu_prepared_key_set_checksum;
    std::string cpu_prepared_build_set_checksum;
    std::int64_t environment_mixed_invalid = -1;
    std::int64_t mixed_persisted_identity = -1;
    std::int64_t operational_identity_conflict = -1;
    std::int64_t stats_saturated = -1;
    std::int64_t target_rows = -1;
    std::int64_t persisted_index_rows = -1;
    std::int64_t missing_persisted_index_rows = -1;
    std::int64_t recomputed_target_index_rows = -1;
    std::int64_t persisted_generation_read_sessions = -1;
    std::int64_t persisted_relation_opens = -1;
    std::int64_t persisted_index_scans = -1;
    std::int64_t persisted_manifest_rows_read = -1;
    std::int64_t persisted_manifest_bytes_read = -1;
    std::int64_t persisted_format5_manifest_contract_valid = -1;
    std::int64_t persisted_format6_contract_valid = -1;
    std::int64_t persisted_format_version = -1;
    std::int64_t cold_index_schema_valid = -1;
    std::int64_t cold_index_route_empty = -1;
    std::int64_t cold_index_route_selective = -1;
    std::int64_t cold_index_route_full = -1;
    std::int64_t cold_index_route_candidate_rows = -1;
    std::int64_t cold_index_route_full_rows = -1;
    std::int64_t cold_index_route_candidate_bytes = -1;
    std::int64_t cold_index_route_full_bytes = -1;
    std::int64_t cold_index_route_selective_cost = -1;
    std::int64_t cold_index_route_full_cost = -1;
    std::int64_t cold_index_payload_rows_requested = -1;
    std::int64_t cold_index_payload_rows_delivered = -1;
    std::int64_t cold_index_payload_rows_scanned = -1;
    std::int64_t cold_index_payload_rows_skipped = -1;
    std::int64_t cold_index_payload_bytes_requested = -1;
    std::int64_t cold_index_payload_bytes_delivered = -1;
    std::int64_t cold_index_payload_bytes_scanned = -1;
    std::int64_t format5_reader_capacity_accounting_passes = -1;
    std::int64_t format5_reader_global_position_storage_preparations = -1;
    std::int64_t format5_reader_global_position_storage_allocations = -1;
    std::int64_t format5_reader_position_append_growth_reservations = -1;
    std::int64_t format5_reader_retained_position_count = -1;
    std::int64_t format5_reader_retained_position_capacity = -1;
    std::int64_t format5_reader_directory_capacity_bytes = -1;
    std::int64_t format5_reader_query_hash_capacity_bytes = -1;
    std::int64_t format5_reader_query_record_capacity_bytes = -1;
    std::int64_t format5_reader_ordinal_route_capacity_bytes = -1;
    std::int64_t format5_reader_selected_extent_capacity_bytes = -1;
    std::int64_t format5_reader_current_selected_control_capacity_bytes = -1;
    std::int64_t format5_reader_global_position_capacity_bytes = -1;
    std::int64_t format5_reader_control_payload_capacity_bytes = -1;
    std::int64_t format5_reader_owned_capacity_bytes = -1;
    std::int64_t format5_reader_admitted_bytes = -1;
    std::int64_t format5_reader_exact_position_image_checks = -1;
    std::int64_t format5_reader_exact_position_image_failures = -1;
    std::int64_t format5_reader_retained_layout_checks = -1;
    std::int64_t format5_reader_retained_layout_failures = -1;
    std::int64_t fragment_native_waves = -1;
    std::int64_t fragment_native_queries = -1;
    std::int64_t fragment_native_occurrence_fill_ns = -1;
    std::int64_t fragment_native_occurrence_radix_ns = -1;
    std::int64_t fragment_native_unique_plan_build_ns = -1;
    std::int64_t fragment_native_query_plan_radix_fill_ns = -1;
    std::int64_t fragment_native_control_count_pass_ns = -1;
    std::int64_t fragment_native_rejected_control_validation_ns = -1;
    std::int64_t fragment_native_orientation_layout_ns = -1;
    std::int64_t fragment_native_direct_emission_ns = -1;
    std::int64_t fragment_native_local_sort_ns = -1;
    std::int64_t fragment_native_tie_normalization_ns = -1;
    std::int64_t fragment_native_control_count_bytes = -1;
    std::int64_t fragment_native_rejected_control_validation_bytes = -1;
    std::int64_t fragment_native_pass2_position_bytes = -1;
    std::int64_t fragment_native_pass3_position_bytes = -1;
    std::int64_t fragment_native_anchors_constructed = -1;
    std::int64_t fragment_native_local_sort_bytes = -1;
    std::int64_t fragment_native_tie_normalization_bytes = -1;
    std::int64_t fragment_native_exact_final_bytes = -1;
    std::int64_t fragment_native_peak_exact_final_bytes = -1;
    std::int64_t fragment_native_global_sort_calls_invoked = -1;
    std::int64_t fragment_native_global_sort_calls_skipped = -1;
    std::int64_t fragment_native_candidate_anchor_bytes = -1;
    std::int64_t fragment_native_plan_ordinal_bytes = -1;
    std::int64_t fragment_native_compaction_bytes = -1;
    std::int64_t fragment_native_final_copy_bytes = -1;
    std::int64_t fragment_native_peak_occurrence_capacity_bytes = -1;
    std::int64_t fragment_native_peak_query_plan_capacity_bytes = -1;
    std::int64_t fragment_native_peak_query_group_offset_capacity_bytes = -1;
    std::int64_t fragment_native_peak_query_membership_filter_capacity_bytes = -1;
    std::int64_t fragment_native_peak_query_radix_capacity_bytes = -1;
    std::int64_t fragment_native_peak_occurrence_count_capacity_bytes = -1;
    std::int64_t fragment_native_peak_block_layout_capacity_bytes = -1;
    std::int64_t fragment_native_peak_block_evidence_capacity_bytes = -1;
    std::int64_t fragment_native_occurrence_allocations = -1;
    std::int64_t fragment_native_query_plan_allocations = -1;
    std::int64_t fragment_native_query_group_offset_allocations = -1;
    std::int64_t fragment_native_query_membership_filter_allocations = -1;
    std::int64_t fragment_native_query_radix_allocations = -1;
    std::int64_t fragment_native_occurrence_count_allocations = -1;
    std::int64_t fragment_native_block_layout_allocations = -1;
    std::int64_t fragment_native_block_evidence_allocations = -1;
    std::int64_t fragment_native_query_occurrences = -1;
    std::int64_t fragment_native_unique_query_plans = -1;
    std::int64_t fragment_native_packed_occurrence_waves = -1;
    std::int64_t fragment_native_wide_occurrence_waves = -1;
    std::int64_t fragment_native_compact_direct_lookup_ns = -1;
    std::int64_t fragment_native_compact_direct_selection_shards = -1;
    std::int64_t fragment_native_compact_direct_shard_query_layouts = -1;
    std::int64_t fragment_native_compact_direct_layout_shards_completed = -1;
    std::int64_t fragment_native_compact_direct_emission_shards_completed = -1;
    std::int64_t fragment_native_compact_direct_posting_visit_passes = -1;
    std::int64_t fragment_native_compact_direct_selected_postings = -1;
    std::int64_t fragment_native_compact_direct_layout_postings_visited = -1;
    std::int64_t fragment_native_compact_direct_emission_postings_visited = -1;
    std::int64_t fragment_native_compact_direct_target_control_scan_bytes = -1;
    std::int64_t fragment_native_compact_direct_target_control_scan_passes = -1;
    std::int64_t fragment_native_compact_direct_fallbacks = -1;
    std::int64_t fragment_native_compact_direct_traversal_rank_checkpoints_examined = -1;
    std::int64_t fragment_native_compact_direct_traversal_membership_words_examined = -1;
    std::int64_t fragment_native_compact_direct_rejected_posting_reads = -1;
    std::int64_t fragment_native_evidence_saturated = -1;
    std::int64_t format5_reader_memory_evidence_saturated = -1;
    nlohmann::json format5_reader_memory_evidence = nullptr;
    std::int64_t format6_directory_rows = -1;
    std::int64_t format6_directory_bytes = -1;
    std::int64_t format6_control_rows = -1;
    std::int64_t format6_control_bytes = -1;
    std::int64_t format6_posting_rows = -1;
    std::int64_t format6_posting_bytes = -1;
    std::int64_t format6_posting_ranges_delivered = -1;
    std::int64_t format6_selected_block_count = -1;
    std::int64_t format6_accepted_hash_count = -1;
    std::int64_t format6_retained_position_count = -1;
    std::int64_t format6_authorized_posting_bytes = -1;
    std::int64_t format6_peak_admitted_bytes = -1;
    std::int64_t format6_retained_owner_bytes = -1;
    std::int64_t format6_admitted_bytes = -1;
    std::int64_t format6_failures = -1;
    std::int64_t format6_saturated = -1;
    nlohmann::json format6_reader_evidence = nullptr;
    std::int64_t persisted_gpu_operation_evidence_schema_valid = -1;
    std::int64_t persisted_gpu_operation_evidence_accepted = -1;
    std::int64_t persisted_gpu_operation_evidence_complete = -1;
    std::int64_t persisted_gpu_operation_evidence_saturated = -1;
    std::int64_t persisted_gpu_dropped_operation_count = -1;
    std::int64_t persisted_gpu_operation_count = -1;
    std::int64_t persisted_gpu_accepted_operation_count = -1;
    std::int64_t persisted_gpu_job_count = -1;
    std::int64_t persisted_gpu_accepted_job_count = -1;
    nlohmann::json persisted_gpu_operation_evidence = nullptr;
    std::int64_t cold_index_stats_saturated = -1;
    std::int64_t extent_crc_raw_bytes_hashed = -1;
    std::int64_t extent_crc_combined_bytes = -1;
    std::int64_t extent_crc_combine_rows = -1;
    std::int64_t gpu_persisted_resident_uploads = -1;
    std::int64_t gpu_persisted_resident_cache_hits = -1;
    std::int64_t gpu_seed_dispatches = -1;
    std::int64_t gpu_chain_dispatches = -1;
    std::int64_t gpu_chain_backtrack_dispatches = -1;
    std::int64_t gpu_extend_dispatches = -1;
    std::int64_t backend_declines = -1;
    std::int64_t backend_fallbacks = -1;
    std::int64_t query_sketch_host_wall_ns = -1;
    std::int64_t target_index_build_host_wall_ns = -1;
    std::int64_t host_query_hash_readbacks = -1;
    std::int64_t host_query_symbol_packing_calls = -1;
    std::int64_t host_target_symbol_packing_calls = -1;
    std::int64_t host_alignment_job_classification_calls = -1;
    std::int64_t host_cigar_construction_calls = -1;
    std::uint64_t measured_region_ns = 0;
    std::uint64_t peak_postgres_rss_kb = 0;
    std::uint64_t peak_minimap2_rss_kb = 0;
    std::uint64_t peak_bench_rss_kb = 0;
    std::uint64_t min_tmp_available_kb = 0;
    double peak_swap_used_mb = 0.0;
    std::int64_t just_index_restart_performed = -1;
    std::string pre_restart_postmaster_start_time;
    std::string post_restart_postmaster_start_time;
    std::int64_t pre_restart_backend_pid = -1;
    std::int64_t post_restart_backend_pid = -1;
    std::int64_t pre_restart_database_oid = -1;
    std::int64_t post_restart_database_oid = -1;
    std::int64_t cold_restart_identity_changed = -1;
    std::int64_t pre_restart_persisted_generation_id = -1;
    std::int64_t pre_restart_persisted_gpu_abi_version = -1;
    std::string pre_restart_persisted_source_identity;
    std::int64_t pre_restart_persisted_format_version = -1;
    std::int64_t post_restart_persisted_format_version = -1;
    nlohmann::json pre_restart_format6_ready_audit = nullptr;
    nlohmann::json post_restart_format6_ready_audit = nullptr;
    std::int64_t format6_restart_audit_accepted = -1;
    // Dedicated no-index lifecycle evidence.  Do not overload the persisted
    // just_index fields: no_index proves an untimed base-data load with no
    // sidecar, a post-load PostgreSQL restart, and an index-free fresh backend
    // before the transient index+map statement begins.
    std::int64_t cold_no_index_restart_performed = -1;
    std::int64_t cold_no_index_restart_identity_changed = -1;
    std::string cold_no_index_pre_restart_postmaster_start_time;
    std::string cold_no_index_post_restart_postmaster_start_time;
    std::int64_t cold_no_index_pre_restart_backend_pid = -1;
    std::int64_t cold_no_index_post_restart_backend_pid = -1;
    std::int64_t cold_no_index_pre_restart_database_oid = -1;
    std::int64_t cold_no_index_post_restart_database_oid = -1;
    std::int64_t cold_no_index_pre_restart_target_rows = -1;
    std::int64_t cold_no_index_post_restart_target_rows = -1;
    std::int64_t cold_no_index_pre_restart_mpf_rows = -1;
    std::int64_t cold_no_index_pre_restart_mps_rows = -1;
    std::int64_t cold_no_index_post_restart_mpf_rows = -1;
    std::int64_t cold_no_index_post_restart_mps_rows = -1;
    std::int64_t cold_no_index_pre_restart_async_jobs = -1;
    std::int64_t cold_no_index_post_restart_async_jobs = -1;
    std::int64_t cold_no_index_measured_async_jobs = -1;
    std::int64_t cold_no_index_pre_restart_gucs_off = -1;
    std::int64_t cold_no_index_post_restart_gucs_off = -1;
    std::int64_t cold_no_index_measured_gucs_off = -1;
    ColdNoIndexPolicyAbsenceEvidence cold_no_index_pre_restart_mpf{};
    ColdNoIndexPolicyAbsenceEvidence cold_no_index_pre_restart_mps{};
    ColdNoIndexPolicyAbsenceEvidence cold_no_index_post_restart_mpf{};
    ColdNoIndexPolicyAbsenceEvidence cold_no_index_post_restart_mps{};
    ColdNoIndexPolicyAbsenceEvidence cold_no_index_measured_mpf{};
    ColdNoIndexPolicyAbsenceEvidence cold_no_index_measured_mps{};
    std::int64_t cold_no_index_lifecycle_accepted = 0;
    std::int64_t persisted_policy_audit_accepted = -1;
    std::int64_t audit_selected_minimizer_is_sequence = -1;
    std::int64_t audit_mpf_rows = -1;
    std::int64_t audit_mps_rows = -1;
    std::int64_t audit_mpf_update_on_insert = -1;
    std::int64_t audit_mps_update_on_insert = -1;
    std::int64_t audit_mpf_active_read = -1;
    std::int64_t audit_mps_active_read = -1;
    std::int64_t audit_async_persist_jobs_waiting = -1;
    std::int64_t audit_index_rows_wait_attempts = 0;
    std::int64_t mps_build_backend_pid = -1;
    std::int64_t mps_build_db_oid = -1;
    std::int64_t mps_build_relfilenode = -1;
    std::int64_t mps_build_sequence_rowno = -1;
    std::int64_t mps_build_total_run_count = -1;
    std::int64_t mps_build_resident_runs = -1;
    std::int64_t mps_build_file_runs = -1;
    std::int64_t mps_build_resident_run_bytes = -1;
    std::int64_t mps_build_resident_peak_bytes = -1;
    std::int64_t mps_build_spilled_run_bytes = -1;
    std::int64_t mps_build_postgres_temp_file_bytes = -1;
    std::int64_t mps_build_postgres_temp_file_count = -1;
    std::int64_t mps_build_merge_reader_buffer_bytes = -1;
    std::int64_t mps_build_merge_pass_count = -1;
    std::int64_t mps_build_spill_count_from_pressure = -1;
    std::int64_t mps_build_resident_demotions = -1;
    std::int64_t mps_build_position_spill_bytes = -1;
    std::int64_t mps_build_position_spill_count = -1;
    std::int64_t mps_build_shared_generation = -1;
    std::int64_t mps_build_worker_build_ns = -1;
    std::int64_t mps_build_pipeline_mode = -1;
    std::int64_t mps_build_max_concurrency = -1;
    std::int64_t mps_build_index_memory_cap_mb = -1;
    std::int64_t mps_build_lazy_worker_shareable = -1;
    std::array<std::int64_t, kAlignmentSnapshotColumnCount>
        alignment_snapshot_numeric_columns = [] {
            std::array<std::int64_t, kAlignmentSnapshotColumnCount> values{};
            values.fill(-1);
            return values;
        }();
    // Lossless copy of the exact same-invocation SRF result.  The selected
    // scalar fields above drive fail-closed acceptance; this complete copy is
    // the durable evidence written to report.json so later analysis never has
    // to trust a lossy projection of the proof.
    it::rowset_t alignment_snapshot_rows{};
};

static_assert(
    E2eOutput::kAlignmentSnapshotColumnCount
    == E2eOutput::kAlignmentSnapshotV1ColumnCount + 1);

[[nodiscard]] bool e2e_expects_format6(
    const E2eInput& in,
    std::string_view minimizer_index) noexcept;
[[nodiscard]] nlohmann::json capture_format6_ready_audit(
    const it::connection_test_context& connection,
    const std::string& table,
    std::string_view minimizer_index,
    std::int64_t sequence_rowno,
    const E2eInput& in);

[[nodiscard]] std::optional<JustIndexSetupKind>
e2e_just_index_setup_kind(std::string_view name) {
    if (name == "bioseq_ksw2_scalar_dp_scalar_mm_just_index") {
        return JustIndexSetupKind::Scalar;
    }
    if (name == "bioseq_just_index"
        || name == "bioseq_block_simd_dp_two_stack_nthash_just_index") {
        return JustIndexSetupKind::Simd;
    }
    if (name == "bioseq_block_vulkan_phase_c_frontend_just_index"
               || name == "bioseq_block_gpu_dp_gpu_seed_just_index") {
        return JustIndexSetupKind::Gpu;
    }
    return std::nullopt;
}

[[nodiscard]] E2eJustIndexSetupRequirements
e2e_just_index_setup_requirements(
        const RunConstraints& constraints) {
    E2eJustIndexSetupRequirements requirements{};
    for (const auto& tuple : constraints.execution_tuples) {
        if (tuple.algorithm_name == "minimap2_just_index") {
            requirements.minimap2 = true;
            continue;
        }
        const auto setup_kind =
            e2e_just_index_setup_kind(tuple.algorithm_name);
        if (!setup_kind.has_value()) {
            if (std::string_view{tuple.algorithm_name}.ends_with("_just_index")) {
                throw std::invalid_argument(fmt::format(
                    "unsupported E2E just_index execution tuple '{}'",
                    tuple.algorithm_name));
            }
            continue;
        }
        const auto policy_it = tuple.params.find("minimizer_index");
        if (policy_it == tuple.params.end() || !policy_it->second.is_string()) {
            throw std::invalid_argument(fmt::format(
                "E2E just_index tuple '{}' has no resolved string "
                "minimizer_index parameter",
                tuple.algorithm_name));
        }
        const std::string_view policy = policy_it->second.as_string();
        bool* required = nullptr;
        if (policy == kMinimizersPerFragmentIndex) {
            switch (*setup_kind) {
                case JustIndexSetupKind::Scalar:
                    required = &requirements.scalar_per_fragment;
                    break;
                case JustIndexSetupKind::Simd:
                    required = &requirements.simd_per_fragment;
                    break;
                case JustIndexSetupKind::Gpu:
                    required = &requirements.gpu_per_fragment;
                    break;
            }
        } else if (policy == kMinimizersPerSequenceIndex) {
            switch (*setup_kind) {
                case JustIndexSetupKind::Scalar:
                    required = &requirements.scalar_per_sequence;
                    break;
                case JustIndexSetupKind::Simd:
                    required = &requirements.simd_per_sequence;
                    break;
                case JustIndexSetupKind::Gpu:
                    required = &requirements.gpu_per_sequence;
                    break;
            }
        } else {
            throw std::invalid_argument(fmt::format(
                "E2E just_index tuple '{}' resolved unsupported "
                "minimizer_index '{}'",
                tuple.algorithm_name,
                policy));
        }
        *required = true;
    }
    return requirements;
}

[[nodiscard]] E2eSetupSelection e2e_setup_selection_for(
        const E2eJustIndexSetupRequirements& requirements) noexcept {
    return E2eSetupSelection{
        .scalar_just_index = requirements.scalar_per_fragment
            || requirements.scalar_per_sequence,
        .simd_just_index = requirements.simd_per_fragment
            || requirements.simd_per_sequence,
        .gpu_just_index = requirements.gpu_per_fragment
            || requirements.gpu_per_sequence,
        .minimap2_just_index = requirements.minimap2,
    };
}

void preflight_e2e_resources(it::connection_test_context& conn,
                             std::string_view size_label,
                             std::int64_t target_bp) {
    constexpr std::uintmax_t kGiB = 1024ull * 1024ull * 1024ull;
    if (target_bp < kPerSeqIndexPreflightThresholdBp) return;

    const auto tmp_free = std::filesystem::space("/tmp").available;
    const auto rows = it::query_rows(conn, "SHOW data_directory");
    const std::filesystem::path pg_data_dir =
        (rows.empty() || !rows[0][0].has_value()) ? "/" : rows[0][0].value();
    const auto pg_free = std::filesystem::space(pg_data_dir).available;
    constexpr std::uintmax_t kRequiredFree = 50ull * kGiB;
    if (tmp_free < kRequiredFree || pg_free < kRequiredFree) {
        bioseq_log<ERR>("[bench_e2e] {} requires >=50 GiB free in /tmp and the PG data "
             "directory before minimizers_per_sequence external-sort build "
             "(tmp_free={} GiB, pg_free={} GiB)",
             size_label,
             static_cast<unsigned long long>(tmp_free / kGiB),
             static_cast<unsigned long long>(pg_free / kGiB));
        std::abort();
    }
}

// ---- small JSON helpers (same shape as bench_v10_main) ----
[[nodiscard]] std::int64_t jint(simdjson::dom::element d, std::string_view k, std::int64_t fb) {
    std::int64_t v = fb; (void)(d[k].get(v) == simdjson::SUCCESS); return v; }
[[nodiscard]] double jdbl(simdjson::dom::element d, std::string_view k, double fb) {
    double v = fb; auto e = d[k]; if (!e.error()) { if (e.value_unsafe().is<double>()) v = e.value_unsafe().get_double().value_unsafe(); else if (e.value_unsafe().is<std::int64_t>()) v = static_cast<double>(e.value_unsafe().get_int64().value_unsafe()); } return v; }
[[nodiscard]] std::string jstr(simdjson::dom::element d, std::string_view k, std::string_view fb) {
    std::string_view v = fb; (void)(d[k].get(v) == simdjson::SUCCESS); return std::string(v); }
[[nodiscard]] bool jbool(simdjson::dom::element d, std::string_view k, bool fb) {
    bool v = fb; (void)(d[k].get(v) == simdjson::SUCCESS); return v; }

// NO_POSTGRES dna4/dna5 set (the pg `pgseq_supported_alphabets` lives in a
// postgres-only module, so the bench instantiates the same machinery locally).
using bench_supported_alphabets = supported_alphabets<
    alphabet_entry<seq::dna4, "dna4">,
    alphabet_entry<seq::dna5, "dna5">>;

template <class HandlerFn>
inline void dispatch_bench_alphabet(std::string_view name, HandlerFn&& handler) {
    std::string owned{name};
    const bool matched = bench_supported_alphabets::dispatch_alphabet_by_name(
        owned.c_str(),
        [&handler]<class Entry>() {
            using alphabet_t = typename Entry::alphabet_type;
            handler.template operator()<alphabet_t>();
        });
    if (!matched) {
        bioseq_log<ERR>("[bench_e2e] unsupported alphabet '{}'", name);
        std::abort();
    }
}

// ---- §10.0c — zero-copy FASTA emission over the IO sequence_writer ----
template <sequence_alphabet A>
using bench_writer = bioseq::io::sequence_writer<
    A, bioseq::io::sequence_io_file_type::FASTA>;
template <sequence_alphabet A>
using bench_view = bioseq::io::compact_sequence_view<bench_config<A>>;

// One zero-copy view over [unit_off, unit_off+unit_count) of a generated range.
template <sequence_alphabet A>
[[nodiscard]] inline bench_view<A> view_slice(const bench_sequence_range<A>& seq,
        std::size_t unit_off, std::size_t unit_count, std::size_t padding) {
    return bench_view<A>{seq._data.data() + unit_off, unit_count, padding};
}

// ---- §11 — FASTA writers / minimap2 drivers / result parsers ----

/// Write `seq` to `path` as FASTA. record_bp == 0 => one record "<base>0" over the
/// whole range. Otherwise cut into records "<base>0".."<base>N" of `record_bp`
/// bases each via the generic, zero-copy split_units — every record is a
/// compact_sequence_view fed straight to the bounded SIMD IO writer (zero
/// re-pack, RSS-bounded). Bodies are line-wrapped (kFastaLineWidth) so
/// bioseq.load's reader slab stays bounded.
template <sequence_alphabet A>
inline void write_fasta(const std::string& path, const bench_sequence_range<A>& seq,
                        std::string_view base_name, std::size_t record_bp) {
    bioseq::io::writer_options wo = bioseq::io::default_writer_options;
    wo.fasta_line_width = kFastaLineWidth;
    auto writer_or = bench_writer<A>::open(path, wo);
    if (!writer_or) {
        bioseq_log<ERR>("[bench_e2e] cannot open '{}': {}", path, writer_or.error()); std::abort();
    }
    auto& writer = *writer_or;
    const auto emit = [&writer, &path, &seq, base_name](
                          std::size_t idx,
                          std::size_t off,
                          std::size_t cnt,
                          std::size_t pad) {
        if (!writer.write_record_streamed(fmt::format("{}{}", base_name, idx),
                                          view_slice<A>(seq, off, cnt, pad),
                                          kFastaStreamChunkBp)) {
            bioseq_log<ERR>("[bench_e2e] write_record_streamed failed for '{}'", path); std::abort();
        }
    };
    if (record_bp == 0) {
        emit(0, 0, seq.size(), seq.padding());          // one whole-range record
    } else {
        std::size_t idx = 0;                            // split_units asserts unit-aligned
        for (const auto& [off, cnt, pad] : seq.split_units(record_bp))
            emit(idx++, off, cnt, pad);
    }
    if (auto c = writer.close(); !c) {
        bioseq_log<ERR>("[bench_e2e] close '{}': {}", path, c.error()); std::abort();
    }
}

template <sequence_alphabet A>
inline void write_single_record_fasta(const std::string& path, std::string_view name,
                                      const bench_sequence_range<A>& seq) {
    write_fasta<A>(path, seq, name, /*record_bp=*/0);       // one record "<name>0"
}
template <sequence_alphabet A>
inline void write_split_fasta(const std::string& path, std::string_view base,
                              const bench_sequence_range<A>& seq, std::size_t split_bp) {
    write_fasta<A>(path, seq, base, split_bp);
}

template <sequence_alphabet A>
[[nodiscard]] std::string decode_target_window(
        const bench_sequence_range<A>& target,
        const std::size_t begin_bp,
        const std::size_t end_bp) {
    using traits = typename bench_sequence_range<A>::traits;
    if (begin_bp > end_bp || end_bp > target.bp_size()) return {};
    auto iterator = target.template iter<SYMBOL>(
        target.cbegin() + static_cast<std::ptrdiff_t>(
            begin_bp / traits::letters_per_unit));
    for (std::size_t skip = 0; skip < begin_bp % traits::letters_per_unit;
         ++skip) {
        ++iterator;
    }
    std::string result;
    result.reserve(end_bp - begin_bp);
    for (std::size_t position = begin_bp; position < end_bp; ++position) {
        result.push_back(iterator->to_char());
        ++iterator;
    }
    return result;
}

template <sequence_alphabet A>
void write_multi_query_fasta(
        const std::string& path,
        std::span<const E2eQueryTruth> queries)
{
    (void)sizeof(A);
    std::ofstream out(path);
    if (!out) {
        throw std::runtime_error("failed to open multi-query FASTA: " + path);
    }
    for (const E2eQueryTruth& query : queries) {
        out << ">query" << query.id << "\n";
        for (std::size_t pos = 0; pos < query.bases.size(); pos += kFastaLineWidth) {
            out << std::string_view{
                query.bases.data() + pos,
                std::min(kFastaLineWidth, query.bases.size() - pos)} << "\n";
        }
    }
}

/// Run a shell command, abort the bench on non-zero exit. Used for the untimed
/// .mmi build. (Inside the timed region we use run_cmd_capture.)
inline void run_cmd(const std::string& cmd) {
    const int rc = std::system(cmd.c_str());
    if (rc != 0) {
        bioseq_log<ERR>("[bench_e2e] command failed (rc={}): {}", rc, cmd);
        std::abort();
    }
}

/// Run a shell command, capture stdout. Used for minimap2 -> PAF. Returns "" and
/// logs on spawn failure (the algorithm then reports found=false; see R3).
[[nodiscard]] inline std::string run_cmd_capture(const std::string& cmd) {
    std::string out;
    std::FILE* pipe = ::popen(cmd.c_str(), "r");
    if (pipe == nullptr) { bioseq_log<ERR>("[bench_e2e] popen failed: {}", cmd); return out; }
    std::array<char, kPopenBufBytes> buf;
    std::size_t n;
    while ((n = std::fread(buf.data(), 1, buf.size(), pipe)) > 0) out.append(buf.data(), n);
    const int rc = ::pclose(pipe);
    if (rc != 0) {
        bioseq_log<ERR>("[bench_e2e] command failed (rc={}): {}", rc, cmd);
        return {};
    }
    return out;
}

[[nodiscard]] inline std::uint64_t parse_u64_prefix(const std::string& text) {
    const char* begin = text.c_str();
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(begin, &end, 10);
    return end == begin ? 0 : static_cast<std::uint64_t>(parsed);
}

[[nodiscard]] inline double parse_double_prefix(const std::string& text) {
    const char* begin = text.c_str();
    char* end = nullptr;
    const double parsed = std::strtod(begin, &end);
    return end == begin ? 0.0 : parsed;
}

struct ResourceSample {
    std::uint64_t postgres_rss_kb = 0;
    std::uint64_t minimap2_rss_kb = 0;
    std::uint64_t bench_rss_kb = 0;
    std::uint64_t tmp_available_kb = 0;
    double swap_used_mb = 0.0;
};

[[nodiscard]] inline ResourceSample sample_resources() {
    ResourceSample out{};

    const std::string rss = run_cmd_capture(
        "ps -axo comm=,rss= | awk '"
        "$1 ~ /postgres/ {pg += $2} "
        "$1 ~ /minimap2/ {mm += $2} "
        "$1 ~ /bioseq_bench_e2e/ {bench += $2} "
        "END {printf \"%llu %llu %llu\\n\", pg, mm, bench}'");
    unsigned long long pg = 0, mm = 0, bench = 0;
    if (std::sscanf(rss.c_str(), "%llu %llu %llu", &pg, &mm, &bench) == 3) {
        out.postgres_rss_kb = static_cast<std::uint64_t>(pg);
        out.minimap2_rss_kb = static_cast<std::uint64_t>(mm);
        out.bench_rss_kb = static_cast<std::uint64_t>(bench);
    }

    out.tmp_available_kb = parse_u64_prefix(
        run_cmd_capture("df -k /tmp | awk 'NR==2 {print $4}'"));
#if defined(__APPLE__)
    out.swap_used_mb = parse_double_prefix(run_cmd_capture(
        "sysctl -n vm.swapusage | awk "
        "'{for (i=1; i<=NF; ++i) if ($i == \"used\") {v=$(i+2); "
        "gsub(\"M\", \"\", v); print v; exit}}'"));
#elif defined(__linux__)
    out.swap_used_mb = parse_double_prefix(run_cmd_capture(
        "awk '/^SwapTotal:/ {total=$2} /^SwapFree:/ {free=$2} "
        "END {if (total >= free) printf \"%.3f\\n\", (total - free) / 1024.0; "
        "else print 0}' /proc/meminfo"));
#else
    out.swap_used_mb = 0.0;
#endif
    return out;
}

class ScopedResourceMonitor {
public:
    ScopedResourceMonitor() {
        record(sample_resources());
        worker_ = std::thread([this]() {
            while (!stop_.load(std::memory_order_relaxed)) {
                record(sample_resources());
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
        });
    }

    ScopedResourceMonitor(const ScopedResourceMonitor&) = delete;
    ScopedResourceMonitor& operator=(const ScopedResourceMonitor&) = delete;

    ~ScopedResourceMonitor() {
        (void)stop();
    }

    [[nodiscard]] ResourceSample stop() {
        stop_.store(true, std::memory_order_relaxed);
        if (worker_.joinable()) {
            worker_.join();
        }
        record(sample_resources());
        return peaks_;
    }

private:
    void record(const ResourceSample& s) noexcept {
        peaks_.postgres_rss_kb = std::max(peaks_.postgres_rss_kb, s.postgres_rss_kb);
        peaks_.minimap2_rss_kb = std::max(peaks_.minimap2_rss_kb, s.minimap2_rss_kb);
        peaks_.bench_rss_kb = std::max(peaks_.bench_rss_kb, s.bench_rss_kb);
        peaks_.swap_used_mb = std::max(peaks_.swap_used_mb, s.swap_used_mb);
        if (s.tmp_available_kb != 0) {
            peaks_.tmp_available_kb = peaks_.tmp_available_kb == 0
                ? s.tmp_available_kb
                : std::min(peaks_.tmp_available_kb, s.tmp_available_kb);
        }
    }

    std::atomic<bool> stop_{false};
    std::thread worker_{};
    ResourceSample peaks_{};
};

// Defensive int parse: leaves the result 0 on malformed input.
[[nodiscard]] inline std::int64_t parse_i64(std::string_view s) {
    std::int64_t v = 0;
    (void) std::from_chars(s.data(), s.data() + s.size(), v);
    return v;
}

[[nodiscard]] inline std::optional<std::int64_t> parse_i64_exact(
        std::string_view text) noexcept {
    std::int64_t value = 0;
    const auto parsed = std::from_chars(
        text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] inline std::optional<bool> parse_pg_bool_exact(
        std::string_view text) noexcept {
    if (text == "t" || text == "true") return true;
    if (text == "f" || text == "false") return false;
    return std::nullopt;
}

struct BenchmarkCigarOp {
    std::uint64_t length = 0;
    char code = '\0';
};

[[nodiscard]] bool parse_benchmark_cigar(
        std::string_view cigar,
        std::vector<BenchmarkCigarOp>& operations) {
    operations.clear();
    std::uint64_t length = 0;
    bool in_number = false;
    for (const char character : cigar) {
        if (character >= '0' && character <= '9') {
            const std::uint64_t digit = static_cast<std::uint64_t>(
                character - '0');
            if (length > ((std::numeric_limits<std::uint64_t>::max)()
                    - digit) / 10u) {
                return false;
            }
            length = length * 10u + digit;
            in_number = true;
            continue;
        }
        const bool supported = character == 'M' || character == 'I'
            || character == 'D' || character == 'N' || character == '='
            || character == 'X' || character == 'S' || character == 'H';
        if (!supported || !in_number || length == 0) {
            return false;
        }
        operations.push_back(BenchmarkCigarOp{
            .length = length,
            .code = character,
        });
        length = 0;
        in_number = false;
    }
    return !operations.empty() && !in_number;
}

[[nodiscard]] bool is_valid_benchmark_cigar(std::string_view cigar) {
    std::vector<BenchmarkCigarOp> operations;
    return parse_benchmark_cigar(cigar, operations);
}

[[nodiscard]] std::uint32_t benchmark_cigar_op_count(
        std::string_view cigar,
        char wanted_op) {
    std::uint64_t len = 0;
    bool in_number = false;
    std::uint32_t count = 0;
    for (char ch : cigar) {
        if (ch >= '0' && ch <= '9') {
            len = (len * 10u) + static_cast<std::uint64_t>(ch - '0');
            in_number = true;
            continue;
        }
        if (!in_number) {
            return false;
        }
        if (ch == wanted_op && len > 0) {
            ++count;
        }
        len = 0;
        in_number = false;
    }
    return count;
}

[[nodiscard]] bool benchmark_cigar_has_indel(std::string_view cigar) {
    return benchmark_cigar_op_count(cigar, 'I') > 0
        || benchmark_cigar_op_count(cigar, 'D') > 0;
}

[[nodiscard]] bool is_expected_benchmark_cigar(
        std::string_view cigar,
        const E2eInput& in) {
    if (!is_valid_benchmark_cigar(cigar)) {
        return false;
    }
    if (in.require_cigar_indel && !benchmark_cigar_has_indel(cigar)) {
        return false;
    }
    if (benchmark_cigar_op_count(cigar, 'D') < in.min_cigar_deletion_ops) {
        return false;
    }
    if (benchmark_cigar_op_count(cigar, 'I') < in.min_cigar_insertion_ops) {
        return false;
    }
    return true;
}

[[nodiscard]] std::string oriented_query_slice(
        const E2eInput& in,
        const E2eQueryTruth& query,
        const std::int64_t query_start,
        const std::int64_t query_end,
        const bool is_reverse) {
    if (query_start < 0 || query_end < query_start
        || static_cast<std::uint64_t>(query_end)
            > static_cast<std::uint64_t>(query.bases.size())) {
        return {};
    }
    const std::string_view forward{
        query.bases.data() + query_start,
        static_cast<std::size_t>(query_end - query_start)};
    if (!is_reverse) return std::string{forward};

    std::string result;
    result.reserve(forward.size());
    dispatch_bench_alphabet(in.alphabet,
        [&result, forward]<sequence_alphabet A>() {
            for (auto it = forward.rbegin(); it != forward.rend(); ++it) {
                A letter{};
                letter.assign_char(*it);
                result.push_back(seqan3::complement(letter).to_char());
            }
        });
    return result;
}

[[nodiscard]] bool replay_benchmark_cigar(
        std::string_view cigar,
        const E2eInput& in,
        const E2eQueryTruth& query,
        const std::int64_t query_start,
        const std::int64_t query_end,
        const std::int64_t target_start,
        const std::int64_t target_end,
        const bool is_reverse) {
    if (target_start < query.target_window_start || target_end < target_start) {
        return false;
    }
    const std::uint64_t window_offset = static_cast<std::uint64_t>(
        target_start - query.target_window_start);
    const std::uint64_t target_length = static_cast<std::uint64_t>(
        target_end - target_start);
    if (window_offset > query.target_window_bases.size()
        || target_length > query.target_window_bases.size() - window_offset) {
        return false;
    }
    const std::string oriented_query = oriented_query_slice(
        in, query, query_start, query_end, is_reverse);
    if (oriented_query.empty() && query_end != query_start) return false;
    const std::string_view target{
        query.target_window_bases.data() + window_offset,
        static_cast<std::size_t>(target_length)};

    std::vector<BenchmarkCigarOp> operations;
    if (!parse_benchmark_cigar(cigar, operations)) return false;
    std::size_t query_cursor = 0;
    std::size_t target_cursor = 0;
    for (const BenchmarkCigarOp operation : operations) {
        const bool consumes_query = operation.code == 'M'
            || operation.code == '=' || operation.code == 'X'
            || operation.code == 'I' || operation.code == 'S';
        const bool consumes_target = operation.code == 'M'
            || operation.code == '=' || operation.code == 'X'
            || operation.code == 'D' || operation.code == 'N';
        if ((consumes_query
                && operation.length > oriented_query.size() - query_cursor)
            || (consumes_target
                && operation.length > target.size() - target_cursor)) {
            return false;
        }
        if (operation.code == '=' || operation.code == 'X') {
            for (std::uint64_t offset = 0; offset < operation.length; ++offset) {
                const bool equal = oriented_query[
                    query_cursor + static_cast<std::size_t>(offset)]
                    == target[target_cursor + static_cast<std::size_t>(offset)];
                if ((operation.code == '=') != equal) return false;
            }
        }
        if (consumes_query) {
            query_cursor += static_cast<std::size_t>(operation.length);
        }
        if (consumes_target) {
            target_cursor += static_cast<std::size_t>(operation.length);
        }
    }
    return query_cursor == oriented_query.size()
        && target_cursor == target.size();
}

void apply_designed_indel_cigar_mutation(E2eQueryTruth& query,
                                         std::size_t ordinal) {
    if (query.bases.size() < 96) {
        std::fprintf(
            stderr,
            "[bench_e2e] designed indel CIGAR case requires query length >= 96 "
            "(qid=%d len=%zu)\n",
            query.id,
            query.bases.size());
        std::fflush(stderr);
        std::abort();
    }
    const std::size_t del_pos = query.bases.size() / 3u;
    constexpr std::size_t kDeleteLen = 8u;
    query.bases.erase(del_pos, kDeleteLen);

    static constexpr std::string_view kInsertA = "ACGTACGTACGTACGT";
    static constexpr std::string_view kInsertB = "TGCATGCATGCATGCA";
    const std::string_view insert = (ordinal % 2u == 0u) ? kInsertA : kInsertB;
    const std::size_t ins_pos = (query.bases.size() * 2u) / 3u;
    query.bases.insert(ins_pos, insert);
}

void apply_designed_many_gap_cigar_mutation(E2eQueryTruth& query) {
    static constexpr std::array<std::size_t, 20> kDeletePositions{
        40u, 80u, 120u, 160u, 200u, 240u, 280u, 320u, 360u, 400u,
        440u, 480u, 520u, 560u, 600u, 640u, 680u, 720u, 760u, 800u};
    if (query.bases.size() <= kDeletePositions.back()) {
        std::fprintf(
            stderr,
            "[bench_e2e] designed many-gap CIGAR case requires query length > %zu "
            "(qid=%d len=%zu)\n",
            kDeletePositions.back(),
            query.id,
            query.bases.size());
        std::fflush(stderr);
        std::abort();
    }
    for (auto it = kDeletePositions.rbegin(); it != kDeletePositions.rend(); ++it) {
        query.bases.erase(*it, 1u);
    }
}

// bioseq: the custom-scan projects one row per matched target row. Columns are
// [header, tstart, rev]; ORDER BY header. At fragment_count=1 there is exactly
// one target row.
void parse_bioseq_rows(const it::rowset_t& rows, const E2eInput& in, E2eOutput& out) {
    out.expected_match_count = 1;
    out.match_count = static_cast<std::int64_t>(rows.size());   // matched target rows
    if (rows.size() != 1 || in.queries.empty()) { out.found = false; return; }
    const it::row_t& r0 = rows.front();                          // [header, tstart, rev]
    const std::size_t expected_columns = in.cigar_projection ? 8u : 3u;
    if (r0.size() != expected_columns || !r0[1].has_value()
        || !r0[in.cigar_projection ? 5u : 2u].has_value()) {
        out.found = false;
        return;
    }
    const std::string& ts = *r0[1];
    std::int64_t v = 0;
    if (std::from_chars(ts.data(), ts.data() + ts.size(), v).ec != std::errc{}) {
        out.found = false; return;                              // malformed tstart
    }
    out.found         = true;
    out.target_start  = v;
    const auto is_reverse_value = parse_pg_bool_exact(
        *r0[in.cigar_projection ? 5u : 2u]);
    if (!is_reverse_value.has_value()) { out.found = false; return; }
    out.is_reverse = *is_reverse_value;
    out.abs_offset_bp = static_cast<double>(
        std::llabs(out.target_start - in.truth_target_start));
    out.max_abs_offset_bp = out.abs_offset_bp;
    out.correct_match_count =
        out.abs_offset_bp < static_cast<double>(kPositionToleranceBp) ? 1 : 0;
    out.exact_primary_count_accepted = 1;
    out.strand_result_count = 1;
    out.correct_strand_count = out.is_reverse == in.queries.front().truth_is_reverse
        ? 1 : 0;
    if (in.cigar_projection && r0[2].has_value() && r0[3].has_value()
        && r0[4].has_value() && r0[6].has_value() && r0[7].has_value()
        && (*r0[6] == "t" || *r0[6] == "true")) {
        const auto target_end = parse_i64_exact(*r0[2]);
        const auto query_start = parse_i64_exact(*r0[3]);
        const auto query_end = parse_i64_exact(*r0[4]);
        if (is_valid_benchmark_cigar(*r0[7])) out.cigar_result_count = 1;
        if (target_end.has_value() && query_start.has_value()
            && query_end.has_value() && out.correct_match_count == 1
            && out.correct_strand_count == 1
            && is_expected_benchmark_cigar(*r0[7], in)
            && replay_benchmark_cigar(
                *r0[7], in, in.queries.front(), *query_start, *query_end,
                v, *target_end, out.is_reverse)) {
            out.correct_cigar_count = 1;
            out.cigar_replay_count = 1;
        }
    }
    out.found = out.correct_match_count == 1 && out.correct_strand_count == 1
        && (!in.cigar_projection || out.cigar_replay_count == 1);
}

void parse_bioseq_batch_rows(
        const it::rowset_t& rows,
        const E2eInput& in,
        E2eOutput& out,
        const std::int64_t target_coordinate_origin = 0)
{
    std::vector<std::uint8_t> row_counts(in.queries.size(), 0);
    std::vector<double> offsets(in.queries.size(), kNoMatchOffsetSentinel);
    bool malformed_or_extra = rows.size() != in.queries.size();

    for (const it::row_t& row : rows) {
        const std::size_t expected_columns = in.cigar_projection ? 9u : 4u;
        if (row.size() != expected_columns || !row[0].has_value()
            || !row[2].has_value() || !row[in.cigar_projection ? 6u : 3u].has_value()) {
            malformed_or_extra = true;
            continue;
        }
        const auto qid_value = parse_i64_exact(*row[0]);
        const auto target_start_value = parse_i64_exact(*row[2]);
        if (!qid_value.has_value() || !target_start_value.has_value()
            || *qid_value < 0
            || static_cast<std::size_t>(*qid_value) >= in.queries.size()) {
            malformed_or_extra = true;
            continue;
        }
        const auto qid = static_cast<std::size_t>(*qid_value);
        if (row_counts[qid] == (std::numeric_limits<std::uint8_t>::max)()) {
            malformed_or_extra = true;
            continue;
        }
        ++row_counts[qid];
        if (row_counts[qid] != 1) malformed_or_extra = true;
        const std::int64_t target_start = *target_start_value;
        if (target_coordinate_origin < 0
            || in.queries[qid].truth_target_start
                < target_coordinate_origin) {
            malformed_or_extra = true;
            continue;
        }
        const std::int64_t expected_local_target_start =
            in.queries[qid].truth_target_start - target_coordinate_origin;
        const double offset = static_cast<double>(
            std::llabs(target_start - expected_local_target_start));
        offsets[qid] = offset;
        const bool coordinate_correct =
            offset < static_cast<double>(kPositionToleranceBp);
        const auto is_reverse_value = parse_pg_bool_exact(
            row[in.cigar_projection ? 6u : 3u].value());
        if (!is_reverse_value.has_value()) {
            malformed_or_extra = true;
            continue;
        }
        const bool is_reverse = *is_reverse_value;
        ++out.strand_result_count;
        if (is_reverse == in.queries[qid].truth_is_reverse) {
            ++out.correct_strand_count;
        }
        if (qid == 0) {
            out.target_start = target_start + target_coordinate_origin;
            out.is_reverse = is_reverse;
        }
        if (!in.cigar_projection) continue;
        const auto target_end = row[3].has_value()
            ? parse_i64_exact(*row[3]) : std::nullopt;
        const auto query_start = row[4].has_value()
            ? parse_i64_exact(*row[4]) : std::nullopt;
        const auto query_end = row[5].has_value()
            ? parse_i64_exact(*row[5]) : std::nullopt;
        const auto has_cigar_value = row[7].has_value()
            ? parse_pg_bool_exact(row[7].value()) : std::nullopt;
        const bool has_cigar = has_cigar_value.value_or(false);
        if (!target_end.has_value() || !query_start.has_value()
            || !query_end.has_value() || !has_cigar || !row[8].has_value()) {
            malformed_or_extra = true;
            continue;
        }
        const std::string_view cigar = row[8].value();
        if (is_valid_benchmark_cigar(cigar)) ++out.cigar_result_count;
        if (coordinate_correct
            && is_reverse == in.queries[qid].truth_is_reverse
            && is_expected_benchmark_cigar(cigar, in)
            && replay_benchmark_cigar(
                cigar, in, in.queries[qid], *query_start, *query_end,
                target_start + target_coordinate_origin,
                *target_end + target_coordinate_origin, is_reverse)) {
            ++out.correct_cigar_count;
            ++out.cigar_replay_count;
        }
    }

    out.expected_match_count = static_cast<std::int64_t>(in.queries.size());
    out.match_count = static_cast<std::int64_t>(rows.size());
    out.correct_match_count = 0;
    out.max_abs_offset_bp = 0.0;
    for (std::size_t i = 0; i < in.queries.size(); ++i) {
        if (row_counts[i] == 1
            && offsets[i] < static_cast<double>(kPositionToleranceBp)) {
            ++out.correct_match_count;
        }
        out.max_abs_offset_bp = std::max(out.max_abs_offset_bp, offsets[i]);
    }
    out.exact_primary_count_accepted = !malformed_or_extra
        && std::ranges::all_of(row_counts,
            [](const std::uint8_t count) noexcept { return count == 1; })
        ? 1 : 0;
    out.found = out.exact_primary_count_accepted == 1
        && out.correct_match_count == out.expected_match_count
        && out.correct_strand_count == out.expected_match_count
        && (!in.cigar_projection
            || (out.correct_cigar_count == out.expected_match_count
                && out.cigar_replay_count == out.expected_match_count));
    out.abs_offset_bp = out.max_abs_offset_bp;
}

[[nodiscard]] std::optional<std::int64_t> explain_nonnegative_i64(
        simdjson::dom::element value)
{
    std::int64_t signed_value = 0;
    if (value.get(signed_value) == simdjson::SUCCESS
        && signed_value >= 0) {
        return std::optional<std::int64_t>{signed_value};
    }
    std::uint64_t unsigned_value = 0;
    if (value.get(unsigned_value) == simdjson::SUCCESS
        && unsigned_value <= static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max())) {
        return std::optional<std::int64_t>{
            static_cast<std::int64_t>(unsigned_value)};
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::int64_t> explain_bool_flag(
        simdjson::dom::element value)
{
    bool bool_value = false;
    if (value.get(bool_value) == simdjson::SUCCESS) {
        return std::optional<std::int64_t>{bool_value ? 1 : 0};
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::string> explain_string(
        simdjson::dom::element value)
{
    std::string_view text;
    if (value.get(text) != simdjson::SUCCESS) {
        return std::nullopt;
    }
    return std::optional<std::string>{std::string{text}};
}

struct PipelineProofParseState {
    std::set<std::string> seen_fields;
    bool group_found = false;
    bool invalid = false;
};

[[noreturn]] void abort_pipeline_proof_capture(std::string_view reason) {
    std::fprintf(
        stderr,
        "[bench_e2e] PIPELINE-PROOF-CAPTURE-FAIL %.*s\n",
        static_cast<int>(reason.size()),
        reason.data());
    std::fflush(stderr);
    std::abort();
}

void assign_pipeline_explain_metric(
        std::string_view key,
        simdjson::dom::element value,
        E2eOutput& out,
        PipelineProofParseState& state)
{
    const auto claim_field = [&] {
        if (!state.seen_fields.emplace(std::string{key}).second) {
            state.invalid = true;
            return false;
        }
        return true;
    };
    const auto assign_i64 = [&](std::int64_t& destination) {
        if (!claim_field()) {
            return;
        }
        const auto parsed = explain_nonnegative_i64(value);
        if (!parsed.has_value()) {
            state.invalid = true;
            return;
        }
        destination = *parsed;
    };
    const auto assign_bool = [&](std::int64_t& destination) {
        if (!claim_field()) {
            return;
        }
        const auto parsed = explain_bool_flag(value);
        if (!parsed.has_value()) {
            state.invalid = true;
            return;
        }
        destination = *parsed;
    };
    const auto assign_text = [&](std::string& destination) {
        if (!claim_field()) {
            return;
        }
        const auto parsed = explain_string(value);
        if (!parsed.has_value()) {
            state.invalid = true;
            return;
        }
        destination = *parsed;
    };

    if (key == "Requested Backend") {
        assign_text(out.requested_backend);
    } else if (key == "Actual Backend") {
        assign_text(out.actual_backend);
    } else if (key == "Requested Target Index Source") {
        assign_text(out.requested_target_index_source);
    } else if (key == "Actual Target Index Source") {
        assign_text(out.actual_target_index_source);
    } else if (key == "Attempted Alignment Jobs") {
        assign_i64(out.attempted_alignment_jobs);
    } else if (key == "Fully Attested Alignment Jobs") {
        assign_i64(out.fully_attested_alignment_jobs);
    } else if (key == "Unattested Alignment Jobs") {
        assign_i64(out.unattested_alignment_jobs);
    } else if (key == "First Unattested Target Row") {
        assign_i64(out.first_unattested_target_row);
    } else if (key == "First Unattested Query Ordinal") {
        assign_i64(out.first_unattested_query_ordinal);
    } else if (key == "Environment Mixed Invalid") {
        assign_bool(out.environment_mixed_invalid);
    } else if (key == "Mixed Persisted Identity") {
        assign_bool(out.mixed_persisted_identity);
    } else if (key == "Operational Identity Conflict") {
        assign_bool(out.operational_identity_conflict);
    } else if (key == "Stats Saturated") {
        assign_bool(out.stats_saturated);
    } else if (key == "Target Rows") {
        assign_i64(out.target_rows);
    } else if (key == "Persisted Index Rows") {
        assign_i64(out.persisted_index_rows);
    } else if (key == "Missing Persisted Index Rows") {
        assign_i64(out.missing_persisted_index_rows);
    } else if (key == "Recomputed Target Index Rows") {
        assign_i64(out.recomputed_target_index_rows);
    } else if (key == "GPU Seed Dispatches") {
        assign_i64(out.gpu_seed_dispatches);
    } else if (key == "GPU Chain Dispatches") {
        assign_i64(out.gpu_chain_dispatches);
    } else if (key == "GPU Chain Backtrack Dispatches") {
        assign_i64(out.gpu_chain_backtrack_dispatches);
    } else if (key == "GPU Extend Dispatches") {
        assign_i64(out.gpu_extend_dispatches);
    } else if (key == "Backend Declines") {
        assign_i64(out.backend_declines);
    } else if (key == "Backend Fallbacks") {
        assign_i64(out.backend_fallbacks);
    } else if (key == "Query Sketch Host Wall Ns") {
        assign_i64(out.query_sketch_host_wall_ns);
    } else if (key == "Target Index Build Host Wall Ns") {
        assign_i64(out.target_index_build_host_wall_ns);
    } else if (key == "Host Query Hash Readbacks") {
        assign_i64(out.host_query_hash_readbacks);
    } else if (key == "Host Query Symbol Packing Calls") {
        assign_i64(out.host_query_symbol_packing_calls);
    } else if (key == "Host Target Symbol Packing Calls") {
        assign_i64(out.host_target_symbol_packing_calls);
    } else if (key == "Host Alignment Job Classification Calls") {
        assign_i64(out.host_alignment_job_classification_calls);
    } else if (key == "Host CIGAR Construction Calls") {
        assign_i64(out.host_cigar_construction_calls);
    }
}

void parse_pipeline_proof_object(
        simdjson::dom::element element,
        E2eOutput& out,
        PipelineProofParseState& state)
{
    simdjson::dom::object object_value;
    if (element.get(object_value) != simdjson::SUCCESS) {
        state.invalid = true;
        return;
    }
    for (auto field : object_value) {
        assign_pipeline_explain_metric(field.key, field.value, out, state);
    }
}

void walk_pipeline_explain_json(
        simdjson::dom::element element,
        E2eOutput& out,
        PipelineProofParseState& state)
{
    const simdjson::dom::element_type type = element.type();
    if (type == simdjson::dom::element_type::ARRAY) {
        simdjson::dom::array array_value;
        if (element.get(array_value) != simdjson::SUCCESS) {
            return;
        }
        for (simdjson::dom::element child : array_value) {
            walk_pipeline_explain_json(child, out, state);
        }
        return;
    }
    if (type != simdjson::dom::element_type::OBJECT) {
        return;
    }
    simdjson::dom::object object_value;
    if (element.get(object_value) != simdjson::SUCCESS) {
        return;
    }
    for (auto field : object_value) {
        if (field.key == "Bioseq Execution Proof") {
            if (state.group_found) {
                state.invalid = true;
                continue;
            }
            state.group_found = true;
            parse_pipeline_proof_object(field.value, out, state);
            continue;
        }
        walk_pipeline_explain_json(field.value, out, state);
    }
}

[[nodiscard]] bool pipeline_proof_fields_complete(
        const E2eOutput& out) noexcept
{
    const auto nonnegative = [](const std::int64_t value) {
        return value >= 0;
    };
    return !out.requested_backend.empty()
        && !out.actual_backend.empty()
        && !out.requested_target_index_source.empty()
        && !out.actual_target_index_source.empty()
        && nonnegative(out.attempted_alignment_jobs)
        && nonnegative(out.fully_attested_alignment_jobs)
        && nonnegative(out.unattested_alignment_jobs)
        && (out.unattested_alignment_jobs == 0
            || (nonnegative(out.first_unattested_target_row)
                && nonnegative(out.first_unattested_query_ordinal)))
        && nonnegative(out.environment_mixed_invalid)
        && nonnegative(out.mixed_persisted_identity)
        && nonnegative(out.operational_identity_conflict)
        && nonnegative(out.stats_saturated)
        && nonnegative(out.target_rows)
        && nonnegative(out.persisted_index_rows)
        && nonnegative(out.missing_persisted_index_rows)
        && nonnegative(out.recomputed_target_index_rows)
        && nonnegative(out.gpu_seed_dispatches)
        && nonnegative(out.gpu_chain_dispatches)
        && nonnegative(out.gpu_chain_backtrack_dispatches)
        && nonnegative(out.gpu_extend_dispatches)
        && nonnegative(out.backend_declines)
        && nonnegative(out.backend_fallbacks)
        && nonnegative(out.query_sketch_host_wall_ns)
        && nonnegative(out.target_index_build_host_wall_ns)
        && nonnegative(out.host_query_hash_readbacks)
        && nonnegative(out.host_query_symbol_packing_calls)
        && nonnegative(out.host_target_symbol_packing_calls)
        && nonnegative(out.host_alignment_job_classification_calls)
        && nonnegative(out.host_cigar_construction_calls);
}

[[maybe_unused]] void capture_pipeline_explain_metrics(
        const it::connection_test_context& c,
        const std::string& align_sql,
        E2eOutput& out)
{
    // This is a second, untimed statement. Preserve the evidence for
    // diagnostics, but never present it as proof of the measured statement.
    out.pipeline_proof_attached_to_measurement = 0;
    const it::rowset_t explain_rows = it::query_rows(
        c,
        "EXPLAIN (ANALYZE, VERBOSE, FORMAT JSON) " + align_sql);
    if (explain_rows.empty() || explain_rows.front().empty()
        || !explain_rows.front()[0].has_value()) {
        abort_pipeline_proof_capture(
            "canonical pipeline EXPLAIN returned no JSON document");
    }
    simdjson::dom::parser parser;
    simdjson::dom::element doc;
    const std::string& json_text = *explain_rows.front()[0];
    if (parser.parse(json_text).get(doc) != simdjson::SUCCESS) {
        abort_pipeline_proof_capture(
            "canonical pipeline EXPLAIN JSON failed to parse");
    }
    PipelineProofParseState state{};
    walk_pipeline_explain_json(doc, out, state);
    if (!state.group_found || state.invalid
        || !pipeline_proof_fields_complete(out)) {
        abort_pipeline_proof_capture(
            "canonical pipeline EXPLAIN proof is missing, duplicated, or ill-typed");
    }
    out.pipeline_proof_present = 1;
}

struct AlignmentSnapshotIdentity {
    std::int64_t execution_id = 0;
    std::int64_t backend_pid = 0;
    std::int64_t database_oid = 0;
    std::string postmaster_start_time;
};

[[nodiscard]] std::optional<std::int64_t> parse_snapshot_i64(
        const std::optional<std::string>& cell) {
    if (!cell.has_value() || cell->empty()) {
        return std::nullopt;
    }
    std::int64_t value = 0;
    const char* first = cell->data();
    const char* last = first + cell->size();
    const auto parsed = std::from_chars(first, last, value);
    if (parsed.ec != std::errc{} || parsed.ptr != last || value < 0) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] std::optional<std::int64_t> parse_snapshot_bool(
        const std::optional<std::string>& cell) {
    if (!cell.has_value()) return std::nullopt;
    if (*cell == "t" || *cell == "true") return 1;
    if (*cell == "f" || *cell == "false") return 0;
    return std::nullopt;
}

[[nodiscard]] AlignmentSnapshotIdentity capture_prior_alignment_identity(
        const it::connection_test_context& connection) {
    const auto rows = it::query_rows(
        connection,
        "SELECT COALESCE((SELECT execution_id "
        "FROM bioseq.last_alignment_execution_stats() LIMIT 1), 0), "
        "pg_backend_pid()::bigint, "
        "(SELECT oid::bigint FROM pg_database "
        " WHERE datname=current_database()), "
        "pg_postmaster_start_time()::text;");
    if (rows.size() != 1 || rows.front().size() != 4) {
        abort_pipeline_proof_capture(
            "prior alignment snapshot identity has an invalid row shape");
    }
    const auto execution_id = parse_snapshot_i64(rows.front()[0]);
    const auto backend_pid = parse_snapshot_i64(rows.front()[1]);
    const auto database_oid = parse_snapshot_i64(rows.front()[2]);
    if (!execution_id.has_value() || !backend_pid.has_value()
        || !database_oid.has_value() || *backend_pid == 0
        || *database_oid == 0 || !rows.front()[3].has_value()
        || rows.front()[3]->empty()) {
        abort_pipeline_proof_capture(
            "prior alignment snapshot identity is missing or ill-typed");
    }
    return AlignmentSnapshotIdentity{
        .execution_id = *execution_id,
        .backend_pid = *backend_pid,
        .database_oid = *database_oid,
        .postmaster_start_time = *rows.front()[3],
    };
}

[[nodiscard]] bool alignment_stats_boolean_column(
        const std::size_t column) noexcept {
    switch (column) {
        case 10: case 13: case 14: case 15: case 22: case 44: case 141:
            return true;
        default:
            return false;
    }
}

[[nodiscard]] bool alignment_stats_text_column(
        const std::size_t column) noexcept {
    switch (column) {
        case 4: case 5: case 6: case 7: case 9:
        case 16: case 17: case 18: case 19: case 21: case 23:
        case 42: case 43:
            return true;
        default:
            return false;
    }
}

[[nodiscard]] constexpr bool alignment_stats_json_column(
        const std::size_t column) noexcept {
    return column == 149 || column == 150;
}

inline constexpr std::array<std::string_view,
    E2eOutput::kAlignmentSnapshotColumnCount> kAlignmentSnapshotColumnNames{
    "execution_id",
    "backend_pid",
    "database_oid",
    "effective_cpu_workers",
    "gpu_device_uuid",
    "gpu_driver",
    "gpu_pipeline_cache_policy",
    "gpu_pipeline_cache_seed_sha256",
    "gpu_pipeline_cache_seed_size",
    "gpu_pipeline_cache_manifest_checksum",
    "gpu_pipeline_cache_hit",
    "scan_node_count",
    "nested_alignment_executor_count",
    "completed",
    "partial",
    "fully_drained",
    "requested_backend",
    "actual_backend",
    "requested_target_index_source",
    "actual_target_index_source",
    "persisted_generation_id",
    "persisted_source_identity",
    "mixed_persisted_identity",
    "stage",
    "eligible_jobs",
    "executed_host_io_jobs",
    "executed_scalar_cpu_jobs",
    "executed_simd_cpu_jobs",
    "executed_vulkan_gpu_jobs",
    "fallback_jobs",
    "stage_host_wall_ns",
    "stage_gpu_device_ns",
    "target_rows",
    "attempted_alignment_jobs",
    "fully_attested_alignment_jobs",
    "unattested_alignment_jobs",
    "first_unattested_target_row",
    "first_unattested_query_ordinal",
    "cpu_prepared_index_builds",
    "cpu_prepared_index_reuses",
    "cpu_prepared_unique_keys",
    "cpu_prepared_duplicate_build_attempts",
    "cpu_prepared_key_set_checksum",
    "cpu_prepared_build_set_checksum",
    "operational_identity_conflict",
    "persisted_index_rows",
    "missing_persisted_index_rows",
    "recomputed_target_index_rows",
    "persisted_generation_read_sessions",
    "persisted_relation_opens",
    "persisted_index_scans",
    "persisted_head_scans",
    "persisted_head_rows_read",
    "persisted_head_bytes_read",
    "persisted_manifest_directory_scans",
    "persisted_manifest_rows_read",
    "persisted_manifest_bytes_read",
    "persisted_directory_chunks_read",
    "persisted_directory_bytes_read",
    "persisted_metadata_repeated_reads",
    "persisted_metadata_heap_rows_read",
    "persisted_metadata_bytes_read",
    "persisted_payload_heap_rows_read",
    "persisted_payload_bytes_read",
    "persisted_heap_rows_read",
    "persisted_toast_values_read",
    "persisted_detoast_bytes",
    "persisted_explicit_copy_bytes",
    "persisted_wanted_subkeys",
    "persisted_read_subkeys",
    "persisted_wanted_extents",
    "persisted_wanted_chunks",
    "persisted_skipped_rows",
    "persisted_repeated_chunk_reads",
    "persisted_extents_read",
    "persisted_chunks_read",
    "persisted_bytes_read",
    "persisted_bulk_read_calls",
    "persisted_planned_request_groups",
    "persisted_executed_request_groups",
    "persisted_range_scan_calls",
    "persisted_skip_budget_rows",
    "persisted_hash_build_calls",
    "persisted_assembly_ns",
    "pair_sort_calls",
    "host_pair_materialization_calls",
    "host_biological_hash_filter_calls",
    "host_query_hash_readbacks",
    "host_query_symbol_packing_calls",
    "host_target_symbol_packing_calls",
    "host_alignment_job_classification_calls",
    "host_anchor_concatenation_calls",
    "host_cigar_construction_calls",
    "full_segment_hydrations",
    "nthash_vector_batches",
    "nthash_scalar_backend_branches",
    "nthash_scalar_tail_symbols",
    "nthash_masked_tail_lanes",
    "simd_chain_vector_batches",
    "simd_chain_backtrack_vector_batches",
    "simd_chain_scalar_backend_branches",
    "simd_chain_backtrack_scalar_branches",
    "cpu_seed_positions_rejected_outside_collection",
    "gpu_partition_select_dispatches",
    "gpu_partition_anchor_merge_dispatches",
    "gpu_index_build_candidates",
    "gpu_index_build_canonical_emitted",
    "gpu_index_build_overlap_duplicates_rejected",
    "gpu_index_build_noncanonical_extras_rejected",
    "gpu_index_build_surviving_extras",
    "gpu_persisted_resident_uploads",
    "gpu_persisted_resident_cache_hits",
    "gpu_overlap_anchor_duplicates",
    "gpu_seed_dispatches",
    "gpu_chain_dispatches",
    "gpu_chain_backtrack_dispatches",
    "gpu_extend_dispatches",
    "gpu_target_window_request_dispatches",
    "gpu_target_window_logical_units_requested",
    "host_target_window_requests_consumed",
    "host_target_window_physical_units_read",
    "host_target_window_remap_units_filled",
    "host_target_window_packed_bytes_read",
    "gpu_runtime_init_ns",
    "gpu_pipeline_cache_loads",
    "gpu_pipeline_cache_seed_bytes",
    "gpu_pipeline_cache_load_ns",
    "gpu_pipeline_creation_ns",
    "gpu_submit_cpu_ns",
    "gpu_wait_cpu_ns",
    "gpu_ordinal_readback_cpu_ns",
    "gpu_result_readback_cpu_ns",
    "gpu_device_critical_path_ns",
    "gpu_timestamp_query_failures",
    "gpu_queue_submits",
    "gpu_host_waits",
    "gpu_buffer_creates",
    "gpu_memory_allocations",
    "gpu_pipeline_creates",
    "backend_declines",
	"backend_fallbacks",
	"stats_saturated",
	"persisted_source_parent_rows_read",
	"persisted_source_catalog_rows_read",
	"persisted_source_catalog_index_scans",
	"persisted_source_fragment_rows_read",
	"persisted_source_decoded_bp",
	"persisted_legacy_globalize_calls",
	"persisted_legacy_rebase_calls",
	"cold_index_preparation",
	"simd_execution_evidence",
};

[[nodiscard]] constexpr bool fixed_lower_hex(
        std::string_view value,
        const std::size_t expected_size) noexcept {
    if (value.size() != expected_size) return false;
    for (const char ch : value) {
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] constexpr bool nonzero_fixed_lower_hex(
        std::string_view value,
        const std::size_t expected_size) noexcept {
    return fixed_lower_hex(value, expected_size)
        && value.find_first_not_of('0') != std::string_view::npos;
}

struct GpuCacheIdentityGateEvidence {
    std::string_view device_uuid;
    std::string_view driver;
    std::string_view policy;
    std::string_view seed_sha256;
    std::int64_t seed_size = -1;
    std::string_view manifest_checksum;
    std::int64_t hit = -1;
};

[[nodiscard]] constexpr bool gpu_cache_identity_evidence_valid(
        const GpuCacheIdentityGateEvidence& evidence) noexcept {
    return nonzero_fixed_lower_hex(evidence.device_uuid, 32)
        && !evidence.driver.empty()
        && evidence.policy == "immutable_preseed_all_hit"
        && nonzero_fixed_lower_hex(evidence.seed_sha256, 64)
        && evidence.seed_size > 0
        && nonzero_fixed_lower_hex(evidence.manifest_checksum, 16)
        && evidence.hit == 1;
}

struct GpuCacheRuntimeGateEvidence {
    std::int64_t declared_seed_size = -1;
    std::int64_t loads = -1;
    std::int64_t seed_bytes = -1;
    std::int64_t load_ns = -1;
    std::int64_t creation_ns = -1;
    std::int64_t timestamp_failures = -1;
    std::int64_t pipeline_creates = -1;
    std::int64_t backend_declines = -1;
    std::int64_t backend_fallbacks = -1;
};

[[nodiscard]] constexpr bool gpu_cache_runtime_evidence_valid(
        const GpuCacheRuntimeGateEvidence& evidence) noexcept {
    return evidence.declared_seed_size > 0
        && evidence.loads == 1
        && evidence.seed_bytes == evidence.declared_seed_size
        && evidence.load_ns > 0
        && evidence.creation_ns > 0
        && evidence.timestamp_failures == 0
        && evidence.pipeline_creates > 0
        && evidence.backend_declines == 0
        && evidence.backend_fallbacks == 0;
}

consteval bool gpu_cache_gate_contract_self_test() {
    constexpr GpuCacheIdentityGateEvidence valid_identity{
        .device_uuid = "11111111111111111111111111111111",
        .driver = "vk",
        .policy = "immutable_preseed_all_hit",
        .seed_sha256 =
            "11111111111111111111111111111111"
            "11111111111111111111111111111111",
        .seed_size = 64,
        .manifest_checksum = "0000000000001234",
        .hit = 1,
    };
    constexpr auto changed_identity = []<typename T>(
            GpuCacheIdentityGateEvidence evidence,
            T GpuCacheIdentityGateEvidence::* member,
            T value) constexpr {
        evidence.*member = value;
        return evidence;
    };
    struct IdentityCase {
        std::string_view name;
        GpuCacheIdentityGateEvidence evidence;
        bool expected;
    };
    constexpr auto identity_cases = std::to_array<IdentityCase>({
        {"valid immutable hit", valid_identity, true},
        {"isolated policy rejected", changed_identity(valid_identity,
            &GpuCacheIdentityGateEvidence::policy,
            std::string_view{"isolated_miss_diagnostic"}), false},
        {"not-applicable policy rejected", changed_identity(valid_identity,
            &GpuCacheIdentityGateEvidence::policy,
            std::string_view{"not_applicable"}), false},
        {"mixed policy rejected", changed_identity(valid_identity,
            &GpuCacheIdentityGateEvidence::policy,
            std::string_view{"mixed_invalid"}), false},
        {"cache miss rejected", changed_identity(valid_identity,
            &GpuCacheIdentityGateEvidence::hit, std::int64_t{0}), false},
        {"unset hit rejected", changed_identity(valid_identity,
            &GpuCacheIdentityGateEvidence::hit, std::int64_t{-1}), false},
        {"mixed hit rejected", changed_identity(valid_identity,
            &GpuCacheIdentityGateEvidence::hit, std::int64_t{2}), false},
        {"zero UUID rejected", changed_identity(valid_identity,
            &GpuCacheIdentityGateEvidence::device_uuid,
            std::string_view{"00000000000000000000000000000000"}), false},
        {"malformed UUID rejected", changed_identity(valid_identity,
            &GpuCacheIdentityGateEvidence::device_uuid,
            std::string_view{"A1111111111111111111111111111111"}), false},
        {"empty driver rejected", changed_identity(valid_identity,
            &GpuCacheIdentityGateEvidence::driver,
            std::string_view{}), false},
        {"zero SHA rejected", changed_identity(valid_identity,
            &GpuCacheIdentityGateEvidence::seed_sha256,
            std::string_view{
                "00000000000000000000000000000000"
                "00000000000000000000000000000000"}), false},
        {"malformed SHA rejected", changed_identity(valid_identity,
            &GpuCacheIdentityGateEvidence::seed_sha256,
            std::string_view{"1"}), false},
        {"zero seed size rejected", changed_identity(valid_identity,
            &GpuCacheIdentityGateEvidence::seed_size,
            std::int64_t{0}), false},
        {"zero manifest rejected", changed_identity(valid_identity,
            &GpuCacheIdentityGateEvidence::manifest_checksum,
            std::string_view{"0000000000000000"}), false},
        {"malformed manifest rejected", changed_identity(valid_identity,
            &GpuCacheIdentityGateEvidence::manifest_checksum,
            std::string_view{"000000000000123G"}), false},
    });
    for (const auto& test : identity_cases) {
        (void)test.name;
        if (gpu_cache_identity_evidence_valid(test.evidence)
                != test.expected) {
            return false;
        }
    }

    constexpr GpuCacheRuntimeGateEvidence valid_runtime{
        .declared_seed_size = 64,
        .loads = 1,
        .seed_bytes = 64,
        .load_ns = 1,
        .creation_ns = 1,
        .timestamp_failures = 0,
        .pipeline_creates = 1,
        .backend_declines = 0,
        .backend_fallbacks = 0,
    };
    constexpr auto changed_runtime = [](
            GpuCacheRuntimeGateEvidence evidence,
            std::int64_t GpuCacheRuntimeGateEvidence::* member,
            const std::int64_t value) constexpr {
        evidence.*member = value;
        return evidence;
    };
    struct RuntimeCase {
        std::string_view name;
        GpuCacheRuntimeGateEvidence evidence;
        bool expected;
    };
    constexpr auto runtime_cases = std::to_array<RuntimeCase>({
        {"valid runtime proof", valid_runtime, true},
        {"zero declared bytes rejected", changed_runtime(valid_runtime,
            &GpuCacheRuntimeGateEvidence::declared_seed_size, 0), false},
        {"zero loads rejected", changed_runtime(valid_runtime,
            &GpuCacheRuntimeGateEvidence::loads, 0), false},
        {"multiple loads rejected", changed_runtime(valid_runtime,
            &GpuCacheRuntimeGateEvidence::loads, 2), false},
        {"zero seed bytes rejected", changed_runtime(valid_runtime,
            &GpuCacheRuntimeGateEvidence::seed_bytes, 0), false},
        {"mismatched seed bytes rejected", changed_runtime(valid_runtime,
            &GpuCacheRuntimeGateEvidence::seed_bytes, 63), false},
        {"zero load timing rejected", changed_runtime(valid_runtime,
            &GpuCacheRuntimeGateEvidence::load_ns, 0), false},
        {"zero creation timing rejected", changed_runtime(valid_runtime,
            &GpuCacheRuntimeGateEvidence::creation_ns, 0), false},
        {"timestamp failure rejected", changed_runtime(valid_runtime,
            &GpuCacheRuntimeGateEvidence::timestamp_failures, 1), false},
        {"zero pipeline creates rejected", changed_runtime(valid_runtime,
            &GpuCacheRuntimeGateEvidence::pipeline_creates, 0), false},
        {"backend decline rejected", changed_runtime(valid_runtime,
            &GpuCacheRuntimeGateEvidence::backend_declines, 1), false},
        {"backend fallback rejected", changed_runtime(valid_runtime,
            &GpuCacheRuntimeGateEvidence::backend_fallbacks, 1), false},
    });
    for (const auto& test : runtime_cases) {
        (void)test.name;
        if (gpu_cache_runtime_evidence_valid(test.evidence)
                != test.expected) {
            return false;
        }
    }
    return true;
}

static_assert(gpu_cache_gate_contract_self_test());

inline constexpr auto kColdIndexEvidenceKeys =
    std::to_array<std::string_view>({
        "schema", "version", "timing_semantics",
        "decode_timing_available", "transfer_timing_available",
        "target_relation_reuses", "target_relation_extra_opens",
        "parent_random_reads", "policy_heap_opens", "policy_index_opens",
        "source_heap_opens", "source_index_opens", "fragment_heap_opens",
        "sidecar_heap_opens", "sidecar_index_opens",
        "policy_schema_validations", "source_schema_validations",
        "sidecar_schema_validations", "lock_acquires", "lock_releases",
        "snapshot_registers", "route_empty", "route_selective",
        "route_full", "route_candidate_rows", "route_full_rows",
        "route_candidate_bytes", "route_full_bytes", "route_selective_cost",
        "route_full_cost", "memory_reroutes", "payload_bytes_requested",
        "payload_bytes_delivered", "payload_rows_requested",
        "payload_rows_delivered", "payload_rows_scanned",
        "payload_bytes_scanned", "payload_rows_skipped", "heap_tid_adjacent",
        "heap_tid_forward_gap", "heap_tid_backward_jump", "extent_crc_bytes",
        "extent_crc_raw_bytes_hashed", "extent_crc_combined_bytes",
        "extent_crc_combine_rows",
        "row_crc_bytes", "tuple_form_bytes", "policy_ns", "source_ns",
        "bind_ns", "head_ns", "directory_ns", "payload_ns", "decode_ns",
        "transfer_ns", "close_ns", "shared_blks_hit", "shared_blks_read",
        "stats_saturated",
    });
static_assert(kColdIndexEvidenceKeys.size() == 59);

inline constexpr auto kFormat5ReaderMemoryEvidenceKeys =
    std::to_array<std::string_view>({
        "schema", "version", "execution_id", "backend_pid", "database_oid",
        "format5_reader_capacity_accounting_passes",
        "format5_reader_global_position_storage_preparations",
        "format5_reader_global_position_storage_allocations",
        "format5_reader_position_append_growth_reservations",
        "format5_reader_retained_position_count",
        "format5_reader_retained_position_capacity",
        "format5_reader_directory_capacity_bytes",
        "format5_reader_query_hash_capacity_bytes",
        "format5_reader_query_record_capacity_bytes",
        "format5_reader_ordinal_route_capacity_bytes",
        "format5_reader_selected_extent_capacity_bytes",
        "format5_reader_current_selected_control_capacity_bytes",
        "format5_reader_global_position_capacity_bytes",
        "format5_reader_control_payload_capacity_bytes",
        "format5_reader_owned_capacity_bytes",
        "format5_reader_admitted_bytes",
        "format5_reader_exact_position_image_checks",
        "format5_reader_exact_position_image_failures",
        "format5_reader_retained_layout_checks",
        "format5_reader_retained_layout_failures",
        "fragment_native_waves", "fragment_native_queries",
        "fragment_native_occurrence_fill_ns",
        "fragment_native_occurrence_radix_ns",
        "fragment_native_unique_plan_build_ns",
        "fragment_native_query_plan_radix_fill_ns",
        "fragment_native_control_count_pass_ns",
        "fragment_native_rejected_control_validation_ns",
        "fragment_native_orientation_layout_ns",
        "fragment_native_direct_emission_ns",
        "fragment_native_local_sort_ns",
        "fragment_native_tie_normalization_ns",
        "fragment_native_control_count_bytes",
        "fragment_native_rejected_control_validation_bytes",
        "fragment_native_pass2_position_bytes",
        "fragment_native_pass3_position_bytes",
        "fragment_native_anchors_constructed",
        "fragment_native_local_sort_bytes",
        "fragment_native_tie_normalization_bytes",
        "fragment_native_exact_final_bytes",
        "fragment_native_peak_exact_final_bytes",
        "fragment_native_global_sort_calls_invoked",
        "fragment_native_global_sort_calls_skipped",
        "fragment_native_candidate_anchor_bytes",
        "fragment_native_plan_ordinal_bytes",
        "fragment_native_compaction_bytes",
        "fragment_native_final_copy_bytes",
        "fragment_native_peak_occurrence_capacity_bytes",
        "fragment_native_peak_query_plan_capacity_bytes",
        "fragment_native_peak_query_radix_capacity_bytes",
        "fragment_native_peak_occurrence_count_capacity_bytes",
        "fragment_native_peak_block_layout_capacity_bytes",
        "fragment_native_peak_block_evidence_capacity_bytes",
        "fragment_native_occurrence_allocations",
        "fragment_native_query_plan_allocations",
        "fragment_native_query_radix_allocations",
        "fragment_native_occurrence_count_allocations",
        "fragment_native_block_layout_allocations",
        "fragment_native_block_evidence_allocations",
        "fragment_native_query_occurrences",
        "fragment_native_unique_query_plans",
        "fragment_native_packed_occurrence_waves",
        "fragment_native_wide_occurrence_waves",
        "fragment_native_evidence_saturated",
        "format5_reader_memory_evidence_saturated", "stats_saturated",
    });
static_assert(kFormat5ReaderMemoryEvidenceKeys.size() == 71);

inline constexpr auto kFormat5ReaderMemoryEvidenceV4AdditionKeys =
    std::to_array<std::string_view>({
        "provider_configured",
        "provider_captured",
        "provider_persisted",
        "provider_opened",
        "provider_head_scans",
        "provider_head_rows",
        "provider_descriptor_scans",
        "provider_descriptor_rows",
        "payload_tuples_formed",
        "payload_tuples_inserted",
        "payload_tuples_reread",
        "mapped_sink_calls",
        "mapped_sink_bytes",
        "mapped_sink_flushes",
        "mapped_sink_coalescer_capacity_bytes",
        "mapped_sink_pwrite_calls",
        "mapped_sink_short_writes",
        "mapped_sink_eintr_retries",
        "mapped_sink_preallocation_bytes",
        "mapped_sink_preallocation_result",
        "header_crc_bytes",
        "directory_crc_bytes",
        "manifest_crc_bytes",
        "seal_aggregate_payload_crc_bytes",
        "open_aggregate_payload_crc_bytes",
        "lazy_extent_cas_winners",
        "lazy_extent_waiters",
        "lazy_extent_executions",
        "lazy_extent_reuses",
        "lazy_extent_crc_bytes",
        "lazy_extent_state_bytes",
        "lazy_extent_invalid_receipts",
        "lazy_state_layout",
        "mapping_bytes",
        "mapping_count",
        "mapping_resourceowner_releases",
        "file_action_high_water",
        "file_action_identity_mismatches",
        "reaper_descriptors_examined",
        "reaper_relation_rows_deleted",
        "reaper_payload_bytes_deleted",
        "reaper_files_queued",
        "reaper_remaining",
        "orphan_entries_examined",
        "orphan_files_acted",
        "orphan_bytes_acted",
        "orphan_remaining",
        "orphan_cursor_wraps",
        "madvise_choice",
        "madvise_result",
    });
static_assert(kFormat5ReaderMemoryEvidenceV4AdditionKeys.size() == 50);

inline constexpr auto kFormat5ReaderMemoryEvidenceV4Keys = [] {
    std::array<std::string_view, 121u> keys{};
    std::size_t destination = 0u;
    for (const std::string_view key : kFormat5ReaderMemoryEvidenceKeys) {
        keys[destination++] = key;
    }
    for (const std::string_view key
         : kFormat5ReaderMemoryEvidenceV4AdditionKeys) {
        keys[destination++] = key;
    }
    return keys;
}();
static_assert(kFormat5ReaderMemoryEvidenceV4Keys.size() == 121);
static_assert([] {
    for (std::size_t index = 0u;
         index < kFormat5ReaderMemoryEvidenceKeys.size();
         ++index) {
        if (kFormat5ReaderMemoryEvidenceV4Keys[index]
                != kFormat5ReaderMemoryEvidenceKeys[index]) {
            return false;
        }
    }
    return true;
}());

inline constexpr auto kFormat5ReaderMemoryEvidenceV5AdditionKeys =
    std::to_array<std::string_view>({
        "fragment_native_peak_query_group_offset_capacity_bytes",
        "fragment_native_peak_query_membership_filter_capacity_bytes",
        "fragment_native_query_group_offset_allocations",
        "fragment_native_query_membership_filter_allocations",
    });
static_assert(kFormat5ReaderMemoryEvidenceV5AdditionKeys.size() == 4);

inline constexpr auto kFormat5ReaderMemoryEvidenceV5Keys = [] {
    std::array<std::string_view, 125u> keys{};
    std::size_t destination = 0u;
    for (const std::string_view key : kFormat5ReaderMemoryEvidenceV4Keys) {
        keys[destination++] = key;
    }
    for (const std::string_view key
         : kFormat5ReaderMemoryEvidenceV5AdditionKeys) {
        keys[destination++] = key;
    }
    return keys;
}();
static_assert(kFormat5ReaderMemoryEvidenceV5Keys.size() == 125);
static_assert([] {
    for (std::size_t index = 0u;
         index < kFormat5ReaderMemoryEvidenceV4Keys.size();
         ++index) {
        if (kFormat5ReaderMemoryEvidenceV5Keys[index]
                != kFormat5ReaderMemoryEvidenceV4Keys[index]) {
            return false;
        }
    }
    return true;
}());

inline constexpr auto kFormat5ReaderMemoryEvidenceV6AdditionKeys =
    std::to_array<std::string_view>({
        "fragment_native_compact_direct_lookup_ns",
        "fragment_native_compact_direct_selection_shards",
        "fragment_native_compact_direct_shard_query_layouts",
        "fragment_native_compact_direct_layout_shards_completed",
        "fragment_native_compact_direct_emission_shards_completed",
        "fragment_native_compact_direct_posting_visit_passes",
        "fragment_native_compact_direct_selected_postings",
        "fragment_native_compact_direct_layout_postings_visited",
        "fragment_native_compact_direct_emission_postings_visited",
        "fragment_native_compact_direct_target_control_scan_bytes",
        "fragment_native_compact_direct_target_control_scan_passes",
        "fragment_native_compact_direct_fallbacks",
        "fragment_native_compact_direct_traversal_rank_checkpoints_examined",
        "fragment_native_compact_direct_traversal_membership_words_examined",
        "fragment_native_compact_direct_rejected_posting_reads",
    });
static_assert(kFormat5ReaderMemoryEvidenceV6AdditionKeys.size() == 15);

inline constexpr auto kFormat5ReaderMemoryEvidenceV6Keys = [] {
    std::array<std::string_view, 140u> keys{};
    std::size_t destination = 0u;
    for (const std::string_view key : kFormat5ReaderMemoryEvidenceV5Keys) {
        keys[destination++] = key;
    }
    for (const std::string_view key
         : kFormat5ReaderMemoryEvidenceV6AdditionKeys) {
        keys[destination++] = key;
    }
    return keys;
}();
static_assert(kFormat5ReaderMemoryEvidenceV6Keys.size() == 140);
static_assert([] {
    for (std::size_t index = 0u;
         index < kFormat5ReaderMemoryEvidenceV5Keys.size();
         ++index) {
        if (kFormat5ReaderMemoryEvidenceV6Keys[index]
                != kFormat5ReaderMemoryEvidenceV5Keys[index]) {
            return false;
        }
    }
    return true;
}());

inline constexpr auto kFragmentNativeEvidenceCounterKeys =
    std::to_array<std::string_view>({
        "fragment_native_waves", "fragment_native_queries",
        "fragment_native_occurrence_fill_ns",
        "fragment_native_occurrence_radix_ns",
        "fragment_native_unique_plan_build_ns",
        "fragment_native_query_plan_radix_fill_ns",
        "fragment_native_control_count_pass_ns",
        "fragment_native_rejected_control_validation_ns",
        "fragment_native_orientation_layout_ns",
        "fragment_native_direct_emission_ns",
        "fragment_native_local_sort_ns",
        "fragment_native_tie_normalization_ns",
        "fragment_native_control_count_bytes",
        "fragment_native_rejected_control_validation_bytes",
        "fragment_native_pass2_position_bytes",
        "fragment_native_pass3_position_bytes",
        "fragment_native_anchors_constructed",
        "fragment_native_local_sort_bytes",
        "fragment_native_tie_normalization_bytes",
        "fragment_native_exact_final_bytes",
        "fragment_native_peak_exact_final_bytes",
        "fragment_native_global_sort_calls_invoked",
        "fragment_native_global_sort_calls_skipped",
        "fragment_native_candidate_anchor_bytes",
        "fragment_native_plan_ordinal_bytes",
        "fragment_native_compaction_bytes",
        "fragment_native_final_copy_bytes",
        "fragment_native_peak_occurrence_capacity_bytes",
        "fragment_native_peak_query_plan_capacity_bytes",
        "fragment_native_peak_query_group_offset_capacity_bytes",
        "fragment_native_peak_query_membership_filter_capacity_bytes",
        "fragment_native_peak_query_radix_capacity_bytes",
        "fragment_native_peak_occurrence_count_capacity_bytes",
        "fragment_native_peak_block_layout_capacity_bytes",
        "fragment_native_peak_block_evidence_capacity_bytes",
        "fragment_native_occurrence_allocations",
        "fragment_native_query_plan_allocations",
        "fragment_native_query_group_offset_allocations",
        "fragment_native_query_membership_filter_allocations",
        "fragment_native_query_radix_allocations",
        "fragment_native_occurrence_count_allocations",
        "fragment_native_block_layout_allocations",
        "fragment_native_block_evidence_allocations",
        "fragment_native_query_occurrences",
        "fragment_native_unique_query_plans",
        "fragment_native_packed_occurrence_waves",
        "fragment_native_wide_occurrence_waves",
        "fragment_native_compact_direct_lookup_ns",
        "fragment_native_compact_direct_selection_shards",
        "fragment_native_compact_direct_shard_query_layouts",
        "fragment_native_compact_direct_layout_shards_completed",
        "fragment_native_compact_direct_emission_shards_completed",
        "fragment_native_compact_direct_posting_visit_passes",
        "fragment_native_compact_direct_selected_postings",
        "fragment_native_compact_direct_layout_postings_visited",
        "fragment_native_compact_direct_emission_postings_visited",
        "fragment_native_compact_direct_target_control_scan_bytes",
        "fragment_native_compact_direct_target_control_scan_passes",
        "fragment_native_compact_direct_fallbacks",
        "fragment_native_compact_direct_traversal_rank_checkpoints_examined",
        "fragment_native_compact_direct_traversal_membership_words_examined",
        "fragment_native_compact_direct_rejected_posting_reads",
    });
static_assert(kFragmentNativeEvidenceCounterKeys.size() == 62);

inline constexpr auto kFormat6ReaderEvidenceKeys =
    std::to_array<std::string_view>({
        "schema", "version", "execution_id", "backend_pid", "database_oid",
        "format_version", "sessions", "generation_id", "source_identity",
        "source_content_crc64", "compatibility_digest", "directory_rows",
        "directory_bytes", "control_rows", "control_bytes", "posting_rows",
        "posting_bytes", "directory_row_crc_receipts",
        "directory_stream_crc_receipts", "control_row_crc_receipts",
        "control_block_crc_receipts", "control_semantic_receipts",
        "posting_row_crc_receipts", "posting_ranges_delivered",
        "posting_range_crc_receipts", "posting_semantic_receipts",
        "authorized_posting_bytes", "query_count", "selected_block_count",
        "accepted_hash_count",
        "retained_position_count", "directory_phase_peak_bytes",
        "control_phase_peak_bytes", "posting_phase_peak_bytes",
        "peak_admitted_bytes", "retained_owner_bytes", "admitted_bytes",
        "legacy_assembly_calls", "legacy_globalize_calls",
        "legacy_rebase_calls", "recompute_calls",
        "generic_source_fallback_calls", "backend_fallbacks", "failures",
        "saturated", "operational_identity_conflict", "stats_saturated",
    });
static_assert(kFormat6ReaderEvidenceKeys.size() == 47);

inline constexpr auto kFormat6ReadyAuditKeys =
    std::to_array<std::string_view>({
        "schema", "version", "format_version", "database_oid",
        "main_relfilenode", "column_attno", "query_key", "table_name",
        "index_name", "source", "head", "manifest", "metadata", "streams",
    });
inline constexpr auto kFormat6ReadySourceKeys =
    std::to_array<std::string_view>({
        "sequence_rowno", "locator_version", "bucket_idx", "shard_idx",
        "num_fragments", "total_bp_size", "first_sub_row_no", "source_epoch",
        "padding_letters", "alphabet_code", "storage_unit_byte_width",
        "source_content_crc64", "source_identity",
    });
inline constexpr auto kFormat6ReadyHeadKeys =
    std::to_array<std::string_view>({
        "generation_id", "query_key", "source_epoch", "source_identity",
        "source_content_crc64", "compatibility_digest",
        "manifest_address_bound_row_crc64", "manifest_payload_crc64",
        "head_crc64", "dense_seal_present",
    });
inline constexpr auto kFormat6ReadyManifestKeys =
    std::to_array<std::string_view>({
        "generation_id", "source_epoch", "source_identity",
        "source_content_crc64", "target_length", "unique_hash_count",
        "total_posting_count", "compatibility_digest",
        "authenticated_directory_crc64", "generation_crc64",
        "total_control_bytes", "total_posting_bytes",
        "directory_entry_count", "compatibility",
    });
inline constexpr auto kFormat6ReadyCompatibilityKeys =
    std::to_array<std::string_view>({
        "hash_scheme_id", "hash_scheme_version", "alphabet_code",
        "kmer_size", "window_size", "hash_width_bytes",
        "stored_posting_width_bytes", "position_encoding_version",
        "hash_semantics_version", "count_semantics_version",
        "producer_backend_id", "producer_backend_version",
        "controls_per_block", "hash_seed",
    });
inline constexpr auto kFormat6ReadyMetadataKeys =
    std::to_array<std::string_view>({
        "head_rows", "head_bytes", "manifest_rows", "manifest_bytes",
    });
inline constexpr auto kFormat6ReadyStreamKeys =
    std::to_array<std::string_view>({
        "kind", "element_count", "logical_bytes", "row_count",
        "stream_crc64", "exact_catalog_rows", "exact_catalog_bytes",
        "exact_catalog_stream_crc64",
    });

inline constexpr auto kSimdExecutionEvidenceKeys =
    std::to_array<std::string_view>({
        "schema", "version", "requested_policy", "observed_policy",
        "requested_alignment_algorithm", "observed_alignment_algorithm",
        "nthash_vector_batches",
        "nthash_resident_vector_stream_kernel_calls",
        "simd_chaining_exact_policy_calls",
        "simd_chaining_bounded_policy_calls",
        "simd_chaining_bounded_score_batches",
        "simd_chaining_bounded_cutoff_stops",
        "simd_chaining_bounded_candidates_skipped",
        "simd_chaining_policy_rejections",
        "simd_chaining_policy_evidence_saturated",
        "block_aligner_execution_proof",
    });
static_assert(kSimdExecutionEvidenceKeys.size() == 16);

inline constexpr auto kBlockAlignerExecutionProofKeys =
    std::to_array<std::string_view>({
        "present", "accepted", "executed_calls", "nonempty_calls",
        "nonempty_calls_with_simd", "simd_dp_vector_batches",
        "simd_dp_cells", "forbidden_ksw2_calls",
        "forbidden_scalar_biological_fallbacks", "saturated",
    });
static_assert(kBlockAlignerExecutionProofKeys.size() == 10);

inline constexpr auto kPersistedGpuOperationEvidenceKeys =
    std::to_array<std::string_view>({
        "schema", "version", "execution_id", "backend_pid", "database_oid",
        "completed", "partial", "fully_drained", "accepted", "complete",
        "saturated", "dropped_operation_count", "operation_count",
        "accepted_operation_count", "complete_operation_count", "job_count",
        "accepted_job_count", "cache_qualification", "operations",
    });
static_assert(kPersistedGpuOperationEvidenceKeys.size() == 19);

inline constexpr auto kPersistedGpuOperationKeys =
    std::to_array<std::string_view>({
        "operation_identity", "expected_jobs", "proof_jobs",
        "consumed_jobs", "accepted_jobs", "burned_jobs", "pending_jobs",
        "staged_jobs", "published_jobs", "publication_commits",
        "query_pipeline_receipt_count", "shape_pipeline_receipt_count",
        "submission_count", "query_pipeline_operation_identity",
        "shape_pipeline_operation_identity", "query_submission_ordinals",
        "seed_receipt_identity", "cache_token_identity",
        "cache_qualification_identity", "generation_id", "source_identity",
        "cold_load_identity", "source", "environment", "cache_metrics",
        "jobs",
    });
static_assert(kPersistedGpuOperationKeys.size() == 26);

inline constexpr auto kPersistedGpuCacheQualificationKeys =
    std::to_array<std::string_view>({
        "version", "identity", "accepted", "saturated", "archive_count",
        "archive_bytes", "archive_checksum", "receipt_count", "hash_passes",
        "hashed_bytes", "private_files_created", "private_files_unlinked",
        "live_private_paths", "pipeline_filesystem_hash_passes",
        "pipeline_filesystem_hashed_bytes", "helper_protocol_version",
        "helper_build_identity", "helper_source", "helper_executable_size",
        "helper_executable_sha256", "helper_metal_registry_id", "device_uuid",
        "api_version", "driver_version", "vendor_id", "device_id",
        "pipeline_cache_uuid", "seed_receipt_identity", "seed_sha256",
        "seed_size", "manifest_checksum",
    });
static_assert(kPersistedGpuCacheQualificationKeys.size() == 31);

inline constexpr auto kPersistedGpuEnvironmentKeys =
    std::to_array<std::string_view>({
        "device_uuid", "api_version", "driver_version", "vendor_id",
        "device_id", "pipeline_cache_uuid", "seed_sha256", "seed_size",
        "manifest_checksum",
    });
static_assert(kPersistedGpuEnvironmentKeys.size() == 9);

inline constexpr auto kPersistedGpuCacheMetricsKeys =
    std::to_array<std::string_view>({
        "pipeline_cache_loads", "pipeline_cache_seed_bytes",
        "pipeline_cache_load_ns", "pipeline_creates",
        "pipeline_creation_ns",
    });
static_assert(kPersistedGpuCacheMetricsKeys.size() == 5);

inline constexpr auto kPersistedGpuJobKeys =
    std::to_array<std::string_view>({
        "logical_target_row", "logical_query_ordinal", "query_index",
        "query_length", "query_minimizer_count", "query_tile_count",
        "query", "direct", "submissions", "status", "forbidden",
        "coordinate",
    });
static_assert(kPersistedGpuJobKeys.size() == 12);

inline constexpr auto kPersistedGpuQueryKeys =
    std::to_array<std::string_view>({
        "count_dispatches", "scan_dispatches", "emit_dispatches",
        "completed_roundtrips",
    });
static_assert(kPersistedGpuQueryKeys.size() == 4);

inline constexpr auto kPersistedGpuDirectKeys =
    std::to_array<std::string_view>({
        "count_dispatches", "scan_dispatches", "anchor_count",
        "emit_dispatches", "sort_operations", "range_build_dispatches",
        "range_count", "chain_dispatches", "chain_backtrack_dispatches",
        "coordinate_dispatches",
    });
static_assert(kPersistedGpuDirectKeys.size() == 10);

inline constexpr auto kPersistedGpuSubmissionKeys =
    std::to_array<std::string_view>({
        "stage", "operation_identity", "ordinal", "submit_wait_identity",
    });
static_assert(kPersistedGpuSubmissionKeys.size() == 4);

inline constexpr auto kPersistedGpuStatusKeys =
    std::to_array<std::string_view>({
        "count_checks", "emit_checks", "status_or", "result_readbacks",
    });
static_assert(kPersistedGpuStatusKeys.size() == 4);

inline constexpr auto kPersistedGpuForbiddenKeys =
    std::to_array<std::string_view>({
        "coordinate_vote_dispatches", "cpu_or_simd_biological_calls",
        "recompute_calls", "fallback_calls",
    });
static_assert(kPersistedGpuForbiddenKeys.size() == 4);

inline constexpr auto kPersistedGpuCoordinateKeys =
    std::to_array<std::string_view>({
        "valid", "query_start", "query_end", "target_start", "target_end",
        "match_length", "block_length", "chain_score", "seed_count",
        "is_reverse",
    });
static_assert(kPersistedGpuCoordinateKeys.size() == 10);

template<std::size_t N>
[[nodiscard]] bool json_has_exact_keys(
        const nlohmann::json& value,
        const std::array<std::string_view, N>& keys) {
    if (!value.is_object() || value.size() != keys.size()) return false;
    return std::all_of(keys.begin(), keys.end(), [&value](const auto key) {
        return value.contains(std::string{key});
    });
}

[[nodiscard]] std::int64_t required_json_counter(
        const nlohmann::json& object,
        std::string_view key) {
    const auto found = object.find(std::string{key});
    if (found == object.end()
        || !(found->is_number_unsigned() || found->is_number_integer())) {
        abort_pipeline_proof_capture(
            "alignment evidence JSON counter is absent or ill-typed");
    }
    if (found->is_number_unsigned()) {
        const auto value = found->get<std::uint64_t>();
        if (value > static_cast<std::uint64_t>(
                (std::numeric_limits<std::int64_t>::max)())) {
            abort_pipeline_proof_capture(
                "alignment evidence JSON counter exceeds SQL bigint");
        }
        return static_cast<std::int64_t>(value);
    }
    const auto value = found->get<std::int64_t>();
    if (value < 0) {
        abort_pipeline_proof_capture(
            "alignment evidence JSON counter is negative");
    }
    return value;
}

[[nodiscard]] nlohmann::json parse_exact_json_cell(
        const std::optional<std::string>& cell,
        std::string_view diagnostic) {
    if (!cell.has_value()) {
        abort_pipeline_proof_capture(diagnostic);
    }
    try {
        return nlohmann::json::parse(*cell);
    } catch (const nlohmann::json::exception&) {
        abort_pipeline_proof_capture(diagnostic);
    }
}

/// Read one required JSON Boolean or fail the evidence capture closed.
[[nodiscard]] bool required_json_boolean(
        const nlohmann::json& object,
        std::string_view key) {
    const auto found = object.find(std::string{key});
    if (found == object.end() || !found->is_boolean()) {
        abort_pipeline_proof_capture(
            "persisted GPU evidence Boolean is absent or ill-typed");
    }
    return found->get<bool>();
}

/// Read one required JSON string without copying its retained value.
[[nodiscard]] const std::string& required_json_text(
        const nlohmann::json& object,
        std::string_view key) {
    const auto found = object.find(std::string{key});
    if (found == object.end() || !found->is_string()) {
        abort_pipeline_proof_capture(
            "persisted GPU evidence text is absent or ill-typed");
    }
    return found->get_ref<const std::string&>();
}

/// Read one required JSON array from the retained evidence object.
[[nodiscard]] const nlohmann::json& required_json_array(
        const nlohmann::json& object,
        std::string_view key) {
    const auto found = object.find(std::string{key});
    if (found == object.end() || !found->is_array()) {
        abort_pipeline_proof_capture(
            "persisted GPU evidence array is absent or ill-typed");
    }
    return *found;
}

/// Read one required nested JSON object from the retained evidence object.
[[nodiscard]] const nlohmann::json& required_json_object(
        const nlohmann::json& object,
        std::string_view key) {
    const auto found = object.find(std::string{key});
    if (found == object.end() || !found->is_object()) {
        abort_pipeline_proof_capture(
            "persisted GPU evidence object is absent or ill-typed");
    }
    return *found;
}

/// Read one nonzero lower-hex identity with an exact byte-string width.
[[nodiscard]] const std::string& required_json_hex_identity(
        const nlohmann::json& object,
        std::string_view key,
        const std::size_t width) {
    const std::string& value = required_json_text(object, key);
    if (!nonzero_fixed_lower_hex(value, width)) {
        abort_pipeline_proof_capture(
            "persisted GPU evidence identity is zero or malformed");
    }
    return value;
}

/// Read one nonnegative SQL-bigint-compatible JSON array element.
[[nodiscard]] std::int64_t required_json_integer_value(
        const nlohmann::json& value) {
    if (!(value.is_number_unsigned() || value.is_number_integer())) {
        abort_pipeline_proof_capture(
            "persisted GPU evidence array integer is ill-typed");
    }
    if (value.is_number_unsigned()) {
        const auto parsed = value.get<std::uint64_t>();
        if (parsed > static_cast<std::uint64_t>(
                (std::numeric_limits<std::int64_t>::max)())) {
            abort_pipeline_proof_capture(
                "persisted GPU evidence array integer exceeds SQL bigint");
        }
        return static_cast<std::int64_t>(parsed);
    }
    const auto parsed = value.get<std::int64_t>();
    if (parsed < 0) {
        abort_pipeline_proof_capture(
            "persisted GPU evidence array integer is negative");
    }
    return parsed;
}

void capture_versioned_alignment_json(
        const it::rowset_t& rows,
        E2eOutput& out) {
    const nlohmann::json cold = parse_exact_json_cell(
        rows.front()[149], "cold-index evidence JSON failed to parse");
    const bool cold_header_valid = json_has_exact_keys(
            cold, kColdIndexEvidenceKeys)
        && cold["schema"].is_string()
        && cold["schema"] == "bioseq.cold_index_preparation"
        && cold["version"].is_number_integer()
        && cold["version"] == 1
        && cold["timing_semantics"].is_string()
        && cold["timing_semantics"] == "payload_inclusive_wall_v1"
        && cold["decode_timing_available"].is_boolean()
        && !cold["decode_timing_available"].get<bool>()
        && cold["transfer_timing_available"].is_boolean()
        && !cold["transfer_timing_available"].get<bool>()
        && cold["decode_ns"].is_null()
        && cold["transfer_ns"].is_null()
        && cold["stats_saturated"].is_boolean();
    if (!cold_header_valid) {
        abort_pipeline_proof_capture(
            "cold-index evidence JSON has the wrong exact schema or types");
    }
    for (const auto key : kColdIndexEvidenceKeys) {
        if (key == "schema" || key == "version"
            || key == "timing_semantics"
            || key == "decode_timing_available"
            || key == "transfer_timing_available"
            || key == "decode_ns" || key == "transfer_ns"
            || key == "stats_saturated") {
            continue;
        }
        (void)required_json_counter(cold, key);
    }
    out.cold_index_schema_valid = 1;
    out.cold_index_route_empty = required_json_counter(cold, "route_empty");
    out.cold_index_route_selective =
        required_json_counter(cold, "route_selective");
    out.cold_index_route_full = required_json_counter(cold, "route_full");
    out.cold_index_route_candidate_rows =
        required_json_counter(cold, "route_candidate_rows");
    out.cold_index_route_full_rows =
        required_json_counter(cold, "route_full_rows");
    out.cold_index_route_candidate_bytes =
        required_json_counter(cold, "route_candidate_bytes");
    out.cold_index_route_full_bytes =
        required_json_counter(cold, "route_full_bytes");
    out.cold_index_route_selective_cost =
        required_json_counter(cold, "route_selective_cost");
    out.cold_index_route_full_cost =
        required_json_counter(cold, "route_full_cost");
    out.cold_index_payload_rows_requested =
        required_json_counter(cold, "payload_rows_requested");
    out.cold_index_payload_rows_delivered =
        required_json_counter(cold, "payload_rows_delivered");
    out.cold_index_payload_rows_scanned =
        required_json_counter(cold, "payload_rows_scanned");
    out.cold_index_payload_rows_skipped =
        required_json_counter(cold, "payload_rows_skipped");
    out.cold_index_payload_bytes_requested =
        required_json_counter(cold, "payload_bytes_requested");
    out.cold_index_payload_bytes_delivered =
        required_json_counter(cold, "payload_bytes_delivered");
    out.cold_index_payload_bytes_scanned =
        required_json_counter(cold, "payload_bytes_scanned");
    out.extent_crc_raw_bytes_hashed =
        required_json_counter(cold, "extent_crc_raw_bytes_hashed");
    out.extent_crc_combined_bytes =
        required_json_counter(cold, "extent_crc_combined_bytes");
    out.extent_crc_combine_rows =
        required_json_counter(cold, "extent_crc_combine_rows");
    out.cold_index_stats_saturated =
        cold["stats_saturated"].get<bool>() ? 1 : 0;

    const nlohmann::json simd = parse_exact_json_cell(
        rows.front()[150], "SIMD execution evidence JSON failed to parse");
    const auto valid_policy_name = [](const nlohmann::json& value) {
        if (!value.is_string()) return false;
        const std::string& policy = value.get_ref<const std::string&>();
        return policy == "none" || policy == "exact_visited"
            || policy == "bounded_batch_approximate"
            || policy == "mixed_invalid";
    };
    const auto valid_alignment_algorithm_name = [](
            const nlohmann::json& value) {
        if (!value.is_string()) return false;
        const std::string& algorithm =
            value.get_ref<const std::string&>();
        return algorithm == "none" || algorithm == "block_aligner"
            || algorithm == "mixed_invalid";
    };
    const bool simd_header_valid = json_has_exact_keys(
            simd, kSimdExecutionEvidenceKeys)
        && simd["schema"].is_string()
        && simd["schema"] == "bioseq.simd_execution_evidence"
        && simd["version"].is_number_integer()
        && simd["version"] == 2
        && valid_policy_name(simd["requested_policy"])
        && valid_policy_name(simd["observed_policy"])
        && valid_alignment_algorithm_name(
            simd["requested_alignment_algorithm"])
        && valid_alignment_algorithm_name(
            simd["observed_alignment_algorithm"])
        && simd["simd_chaining_policy_evidence_saturated"].is_boolean()
        && json_has_exact_keys(
            simd["block_aligner_execution_proof"],
            kBlockAlignerExecutionProofKeys)
        && simd["block_aligner_execution_proof"]["present"].is_boolean()
        && simd["block_aligner_execution_proof"]["accepted"].is_boolean()
        && simd["block_aligner_execution_proof"]["saturated"].is_boolean();
    if (!simd_header_valid) {
        abort_pipeline_proof_capture(
            "SIMD execution evidence JSON has the wrong exact schema or types");
    }
    const nlohmann::json& block =
        simd["block_aligner_execution_proof"];
    out.simd_evidence_schema_valid = 1;
    out.requested_simd_chaining_policy =
        simd["requested_policy"].get<std::string>();
    out.observed_simd_chaining_policy =
        simd["observed_policy"].get<std::string>();
    out.requested_simd_alignment_algorithm =
        simd["requested_alignment_algorithm"].get<std::string>();
    out.observed_simd_alignment_algorithm =
        simd["observed_alignment_algorithm"].get<std::string>();
    out.nthash_vector_batches =
        required_json_counter(simd, "nthash_vector_batches");
    out.nthash_resident_vector_stream_kernel_calls = required_json_counter(
        simd, "nthash_resident_vector_stream_kernel_calls");
    out.simd_chaining_exact_policy_calls = required_json_counter(
        simd, "simd_chaining_exact_policy_calls");
    out.simd_chaining_bounded_policy_calls = required_json_counter(
        simd, "simd_chaining_bounded_policy_calls");
    out.simd_chaining_bounded_score_batches = required_json_counter(
        simd, "simd_chaining_bounded_score_batches");
    out.simd_chaining_bounded_cutoff_stops = required_json_counter(
        simd, "simd_chaining_bounded_cutoff_stops");
    out.simd_chaining_bounded_candidates_skipped = required_json_counter(
        simd, "simd_chaining_bounded_candidates_skipped");
    out.simd_chaining_policy_rejections = required_json_counter(
        simd, "simd_chaining_policy_rejections");
    out.simd_chaining_policy_evidence_saturated =
        simd["simd_chaining_policy_evidence_saturated"].get<bool>() ? 1 : 0;
    out.block_aligner_execution_proof_present =
        block["present"].get<bool>() ? 1 : 0;
    out.block_aligner_execution_proof_accepted =
        block["accepted"].get<bool>() ? 1 : 0;
    out.block_aligner_executed_calls =
        required_json_counter(block, "executed_calls");
    out.block_aligner_nonempty_calls =
        required_json_counter(block, "nonempty_calls");
    out.block_aligner_nonempty_calls_with_simd =
        required_json_counter(block, "nonempty_calls_with_simd");
    out.block_aligner_simd_dp_vector_batches =
        required_json_counter(block, "simd_dp_vector_batches");
    out.block_aligner_simd_dp_cells =
        required_json_counter(block, "simd_dp_cells");
    out.block_aligner_forbidden_ksw2_calls =
        required_json_counter(block, "forbidden_ksw2_calls");
    out.block_aligner_forbidden_scalar_biological_fallbacks =
        required_json_counter(
            block, "forbidden_scalar_biological_fallbacks");
    out.block_aligner_execution_proof_saturated =
        block["saturated"].get<bool>() ? 1 : 0;
}

void capture_format5_reader_memory_evidence(
        const it::connection_test_context& connection,
        E2eOutput& out) {
    const it::row_t row = it::query_one(
        connection,
        "SELECT bioseq.last_alignment_format5_reader_memory_evidence_v6();");
    if (row.size() != 1) {
        abort_pipeline_proof_capture(
            "Format-5 reader memory evidence has an invalid scalar shape");
    }
    const nlohmann::json evidence = parse_exact_json_cell(
        row.front(), "Format-5 reader memory evidence JSON failed to parse");
    const bool header_valid = json_has_exact_keys(
            evidence, kFormat5ReaderMemoryEvidenceV6Keys)
        && evidence["schema"].is_string()
        && evidence["schema"] == "bioseq.format5_reader_memory_evidence"
        && evidence["version"].is_number_integer()
        && evidence["version"] == 6
        && evidence["fragment_native_evidence_saturated"].is_boolean()
        && evidence["format5_reader_memory_evidence_saturated"].is_boolean()
        && evidence["stats_saturated"].is_boolean();
    if (!header_valid) {
        abort_pipeline_proof_capture(
            "Format-5 reader memory evidence has the wrong exact schema or types");
    }
    for (const std::string_view key : kFragmentNativeEvidenceCounterKeys) {
        (void)required_json_counter(evidence, key);
    }
    for (const std::string_view key
         : kFormat5ReaderMemoryEvidenceV4AdditionKeys) {
        (void)required_json_counter(evidence, key);
    }

    const std::int64_t execution_id =
        required_json_counter(evidence, "execution_id");
    const std::int64_t backend_pid =
        required_json_counter(evidence, "backend_pid");
    const std::int64_t database_oid =
        required_json_counter(evidence, "database_oid");
    if (execution_id != out.alignment_execution_id
        || backend_pid != out.alignment_backend_pid
        || database_oid != out.alignment_database_oid
        || evidence["stats_saturated"].get<bool>()
            != (out.stats_saturated != 0)) {
        abort_pipeline_proof_capture(
            "Format-5 reader memory evidence identity/saturation does not match v6");
    }

    out.format5_reader_capacity_accounting_passes = required_json_counter(
        evidence, "format5_reader_capacity_accounting_passes");
    out.format5_reader_global_position_storage_preparations =
        required_json_counter(
            evidence,
            "format5_reader_global_position_storage_preparations");
    out.format5_reader_global_position_storage_allocations =
        required_json_counter(
            evidence,
            "format5_reader_global_position_storage_allocations");
    out.format5_reader_position_append_growth_reservations =
        required_json_counter(
            evidence,
            "format5_reader_position_append_growth_reservations");
    out.format5_reader_retained_position_count = required_json_counter(
        evidence, "format5_reader_retained_position_count");
    out.format5_reader_retained_position_capacity = required_json_counter(
        evidence, "format5_reader_retained_position_capacity");
    out.format5_reader_directory_capacity_bytes = required_json_counter(
        evidence, "format5_reader_directory_capacity_bytes");
    out.format5_reader_query_hash_capacity_bytes = required_json_counter(
        evidence, "format5_reader_query_hash_capacity_bytes");
    out.format5_reader_query_record_capacity_bytes = required_json_counter(
        evidence, "format5_reader_query_record_capacity_bytes");
    out.format5_reader_ordinal_route_capacity_bytes = required_json_counter(
        evidence, "format5_reader_ordinal_route_capacity_bytes");
    out.format5_reader_selected_extent_capacity_bytes = required_json_counter(
        evidence, "format5_reader_selected_extent_capacity_bytes");
    out.format5_reader_current_selected_control_capacity_bytes =
        required_json_counter(
            evidence,
            "format5_reader_current_selected_control_capacity_bytes");
    out.format5_reader_global_position_capacity_bytes = required_json_counter(
        evidence, "format5_reader_global_position_capacity_bytes");
    out.format5_reader_control_payload_capacity_bytes = required_json_counter(
        evidence, "format5_reader_control_payload_capacity_bytes");
    out.format5_reader_owned_capacity_bytes = required_json_counter(
        evidence, "format5_reader_owned_capacity_bytes");
    out.format5_reader_admitted_bytes = required_json_counter(
        evidence, "format5_reader_admitted_bytes");
    out.format5_reader_exact_position_image_checks = required_json_counter(
        evidence, "format5_reader_exact_position_image_checks");
    out.format5_reader_exact_position_image_failures = required_json_counter(
        evidence, "format5_reader_exact_position_image_failures");
    out.format5_reader_retained_layout_checks = required_json_counter(
        evidence, "format5_reader_retained_layout_checks");
    out.format5_reader_retained_layout_failures = required_json_counter(
        evidence, "format5_reader_retained_layout_failures");
    out.fragment_native_waves = required_json_counter(
        evidence, "fragment_native_waves");
    out.fragment_native_queries = required_json_counter(
        evidence, "fragment_native_queries");
    out.fragment_native_occurrence_fill_ns = required_json_counter(
        evidence, "fragment_native_occurrence_fill_ns");
    out.fragment_native_occurrence_radix_ns = required_json_counter(
        evidence, "fragment_native_occurrence_radix_ns");
    out.fragment_native_unique_plan_build_ns = required_json_counter(
        evidence, "fragment_native_unique_plan_build_ns");
    out.fragment_native_query_plan_radix_fill_ns = required_json_counter(
        evidence, "fragment_native_query_plan_radix_fill_ns");
    out.fragment_native_control_count_pass_ns = required_json_counter(
        evidence, "fragment_native_control_count_pass_ns");
    out.fragment_native_rejected_control_validation_ns =
        required_json_counter(
            evidence, "fragment_native_rejected_control_validation_ns");
    out.fragment_native_orientation_layout_ns = required_json_counter(
        evidence, "fragment_native_orientation_layout_ns");
    out.fragment_native_direct_emission_ns = required_json_counter(
        evidence, "fragment_native_direct_emission_ns");
    out.fragment_native_local_sort_ns = required_json_counter(
        evidence, "fragment_native_local_sort_ns");
    out.fragment_native_tie_normalization_ns = required_json_counter(
        evidence, "fragment_native_tie_normalization_ns");
    out.fragment_native_control_count_bytes = required_json_counter(
        evidence, "fragment_native_control_count_bytes");
    out.fragment_native_rejected_control_validation_bytes =
        required_json_counter(
            evidence, "fragment_native_rejected_control_validation_bytes");
    out.fragment_native_pass2_position_bytes = required_json_counter(
        evidence, "fragment_native_pass2_position_bytes");
    out.fragment_native_pass3_position_bytes = required_json_counter(
        evidence, "fragment_native_pass3_position_bytes");
    out.fragment_native_anchors_constructed = required_json_counter(
        evidence, "fragment_native_anchors_constructed");
    out.fragment_native_local_sort_bytes = required_json_counter(
        evidence, "fragment_native_local_sort_bytes");
    out.fragment_native_tie_normalization_bytes = required_json_counter(
        evidence, "fragment_native_tie_normalization_bytes");
    out.fragment_native_exact_final_bytes = required_json_counter(
        evidence, "fragment_native_exact_final_bytes");
    out.fragment_native_peak_exact_final_bytes = required_json_counter(
        evidence, "fragment_native_peak_exact_final_bytes");
    out.fragment_native_global_sort_calls_invoked = required_json_counter(
        evidence, "fragment_native_global_sort_calls_invoked");
    out.fragment_native_global_sort_calls_skipped = required_json_counter(
        evidence, "fragment_native_global_sort_calls_skipped");
    out.fragment_native_candidate_anchor_bytes = required_json_counter(
        evidence, "fragment_native_candidate_anchor_bytes");
    out.fragment_native_plan_ordinal_bytes = required_json_counter(
        evidence, "fragment_native_plan_ordinal_bytes");
    out.fragment_native_compaction_bytes = required_json_counter(
        evidence, "fragment_native_compaction_bytes");
    out.fragment_native_final_copy_bytes = required_json_counter(
        evidence, "fragment_native_final_copy_bytes");
    out.fragment_native_peak_occurrence_capacity_bytes = required_json_counter(
        evidence, "fragment_native_peak_occurrence_capacity_bytes");
    out.fragment_native_peak_query_plan_capacity_bytes = required_json_counter(
        evidence, "fragment_native_peak_query_plan_capacity_bytes");
    out.fragment_native_peak_query_group_offset_capacity_bytes =
        required_json_counter(
            evidence,
            "fragment_native_peak_query_group_offset_capacity_bytes");
    out.fragment_native_peak_query_membership_filter_capacity_bytes =
        required_json_counter(
            evidence,
            "fragment_native_peak_query_membership_filter_capacity_bytes");
    out.fragment_native_peak_query_radix_capacity_bytes = required_json_counter(
        evidence, "fragment_native_peak_query_radix_capacity_bytes");
    out.fragment_native_peak_occurrence_count_capacity_bytes =
        required_json_counter(
            evidence,
            "fragment_native_peak_occurrence_count_capacity_bytes");
    out.fragment_native_peak_block_layout_capacity_bytes =
        required_json_counter(
            evidence, "fragment_native_peak_block_layout_capacity_bytes");
    out.fragment_native_peak_block_evidence_capacity_bytes =
        required_json_counter(
            evidence, "fragment_native_peak_block_evidence_capacity_bytes");
    out.fragment_native_occurrence_allocations = required_json_counter(
        evidence, "fragment_native_occurrence_allocations");
    out.fragment_native_query_plan_allocations = required_json_counter(
        evidence, "fragment_native_query_plan_allocations");
    out.fragment_native_query_group_offset_allocations = required_json_counter(
        evidence, "fragment_native_query_group_offset_allocations");
    out.fragment_native_query_membership_filter_allocations =
        required_json_counter(
            evidence,
            "fragment_native_query_membership_filter_allocations");
    out.fragment_native_query_radix_allocations = required_json_counter(
        evidence, "fragment_native_query_radix_allocations");
    out.fragment_native_occurrence_count_allocations = required_json_counter(
        evidence, "fragment_native_occurrence_count_allocations");
    out.fragment_native_block_layout_allocations = required_json_counter(
        evidence, "fragment_native_block_layout_allocations");
    out.fragment_native_block_evidence_allocations = required_json_counter(
        evidence, "fragment_native_block_evidence_allocations");
    out.fragment_native_query_occurrences = required_json_counter(
        evidence, "fragment_native_query_occurrences");
    out.fragment_native_unique_query_plans = required_json_counter(
        evidence, "fragment_native_unique_query_plans");
    out.fragment_native_packed_occurrence_waves = required_json_counter(
        evidence, "fragment_native_packed_occurrence_waves");
    out.fragment_native_wide_occurrence_waves = required_json_counter(
        evidence, "fragment_native_wide_occurrence_waves");
    out.fragment_native_compact_direct_lookup_ns = required_json_counter(
        evidence, "fragment_native_compact_direct_lookup_ns");
    out.fragment_native_compact_direct_selection_shards =
        required_json_counter(
            evidence,
            "fragment_native_compact_direct_selection_shards");
    out.fragment_native_compact_direct_shard_query_layouts =
        required_json_counter(
            evidence,
            "fragment_native_compact_direct_shard_query_layouts");
    out.fragment_native_compact_direct_layout_shards_completed =
        required_json_counter(
            evidence,
            "fragment_native_compact_direct_layout_shards_completed");
    out.fragment_native_compact_direct_emission_shards_completed =
        required_json_counter(
            evidence,
            "fragment_native_compact_direct_emission_shards_completed");
    out.fragment_native_compact_direct_posting_visit_passes =
        required_json_counter(
            evidence,
            "fragment_native_compact_direct_posting_visit_passes");
    out.fragment_native_compact_direct_selected_postings =
        required_json_counter(
            evidence,
            "fragment_native_compact_direct_selected_postings");
    out.fragment_native_compact_direct_layout_postings_visited =
        required_json_counter(
            evidence,
            "fragment_native_compact_direct_layout_postings_visited");
    out.fragment_native_compact_direct_emission_postings_visited =
        required_json_counter(
            evidence,
            "fragment_native_compact_direct_emission_postings_visited");
    out.fragment_native_compact_direct_target_control_scan_bytes =
        required_json_counter(
            evidence,
            "fragment_native_compact_direct_target_control_scan_bytes");
    out.fragment_native_compact_direct_target_control_scan_passes =
        required_json_counter(
            evidence,
            "fragment_native_compact_direct_target_control_scan_passes");
    out.fragment_native_compact_direct_fallbacks = required_json_counter(
        evidence, "fragment_native_compact_direct_fallbacks");
    out.fragment_native_compact_direct_traversal_rank_checkpoints_examined =
        required_json_counter(
            evidence,
            "fragment_native_compact_direct_traversal_rank_checkpoints_examined");
    out.fragment_native_compact_direct_traversal_membership_words_examined =
        required_json_counter(
            evidence,
            "fragment_native_compact_direct_traversal_membership_words_examined");
    out.fragment_native_compact_direct_rejected_posting_reads =
        required_json_counter(
            evidence,
            "fragment_native_compact_direct_rejected_posting_reads");
    out.fragment_native_evidence_saturated =
        evidence["fragment_native_evidence_saturated"].get<bool>() ? 1 : 0;
    out.format5_reader_memory_evidence_saturated =
        evidence["format5_reader_memory_evidence_saturated"].get<bool>()
            ? 1 : 0;
    out.format5_reader_memory_evidence = evidence;
}

void capture_format6_reader_evidence(
        const it::connection_test_context& connection,
        E2eOutput& out) {
    const it::row_t row = it::query_one(
        connection,
        "SELECT bioseq.last_alignment_format6_reader_evidence();");
    if (row.size() != 1) {
        abort_pipeline_proof_capture(
            "Format-6 reader evidence has an invalid scalar shape");
    }
    const nlohmann::json evidence = parse_exact_json_cell(
        row.front(), "Format-6 reader evidence JSON failed to parse");
    const bool header_valid = json_has_exact_keys(
            evidence, kFormat6ReaderEvidenceKeys)
        && evidence["schema"].is_string()
        && evidence["schema"] == "bioseq.format6_reader_evidence"
        && evidence["version"].is_number_integer()
        && evidence["version"] == 1
        && evidence["saturated"].is_boolean()
        && evidence["operational_identity_conflict"].is_boolean()
        && evidence["stats_saturated"].is_boolean();
    if (!header_valid) {
        abort_pipeline_proof_capture(
            "Format-6 reader evidence has the wrong exact schema or types");
    }

    for (const auto key : kFormat6ReaderEvidenceKeys) {
        if (key == "schema" || key == "version"
            || key == "source_identity"
            || key == "source_content_crc64"
            || key == "compatibility_digest"
            || key == "saturated"
            || key == "operational_identity_conflict"
            || key == "stats_saturated") {
            continue;
        }
        (void)required_json_counter(evidence, key);
    }
    const std::string& source_identity =
        required_json_text(evidence, "source_identity");
    const std::string& source_content_crc64 =
        required_json_text(evidence, "source_content_crc64");
    const std::string& compatibility_digest =
        required_json_text(evidence, "compatibility_digest");
    if (!fixed_lower_hex(source_identity, 32)
        || !fixed_lower_hex(source_content_crc64, 16)
        || !fixed_lower_hex(compatibility_digest, 64)) {
        abort_pipeline_proof_capture(
            "Format-6 reader evidence identity is malformed");
    }

    const std::int64_t execution_id =
        required_json_counter(evidence, "execution_id");
    const std::int64_t backend_pid =
        required_json_counter(evidence, "backend_pid");
    const std::int64_t database_oid =
        required_json_counter(evidence, "database_oid");
    if (execution_id != out.alignment_execution_id
        || backend_pid != out.alignment_backend_pid
        || database_oid != out.alignment_database_oid
        || evidence["operational_identity_conflict"].get<bool>()
            != (out.operational_identity_conflict != 0)
        || evidence["stats_saturated"].get<bool>()
            != (out.stats_saturated != 0)
        || required_json_counter(evidence, "backend_fallbacks")
            != out.backend_fallbacks
        || required_json_counter(evidence, "recompute_calls")
            != out.recomputed_target_index_rows) {
        abort_pipeline_proof_capture(
            "Format-6 reader evidence identity/counters do not match v1");
    }

    out.format6_directory_rows =
        required_json_counter(evidence, "directory_rows");
    out.format6_directory_bytes =
        required_json_counter(evidence, "directory_bytes");
    out.format6_control_rows =
        required_json_counter(evidence, "control_rows");
    out.format6_control_bytes =
        required_json_counter(evidence, "control_bytes");
    out.format6_posting_rows =
        required_json_counter(evidence, "posting_rows");
    out.format6_posting_bytes =
        required_json_counter(evidence, "posting_bytes");
    out.format6_posting_ranges_delivered =
        required_json_counter(evidence, "posting_ranges_delivered");
    out.format6_selected_block_count =
        required_json_counter(evidence, "selected_block_count");
    out.format6_accepted_hash_count =
        required_json_counter(evidence, "accepted_hash_count");
    out.format6_retained_position_count =
        required_json_counter(evidence, "retained_position_count");
    out.format6_authorized_posting_bytes =
        required_json_counter(evidence, "authorized_posting_bytes");
    out.format6_peak_admitted_bytes =
        required_json_counter(evidence, "peak_admitted_bytes");
    out.format6_retained_owner_bytes =
        required_json_counter(evidence, "retained_owner_bytes");
    out.format6_admitted_bytes =
        required_json_counter(evidence, "admitted_bytes");
    out.format6_failures =
        required_json_counter(evidence, "failures");
    out.format6_saturated =
        evidence["saturated"].get<bool>() ? 1 : 0;
    out.format6_reader_evidence = evidence;
}

/// Validate and retain the immediate cache-qualified persisted-GPU ledger.
void capture_persisted_gpu_operation_evidence(
        const it::row_t& row,
        E2eOutput& out) {
    if (row.size() != 1) {
        abort_pipeline_proof_capture(
            "persisted GPU operation evidence has an invalid scalar shape");
    }
    const nlohmann::json evidence = parse_exact_json_cell(
        row.front(), "persisted GPU operation evidence JSON failed to parse");
    if (!json_has_exact_keys(evidence, kPersistedGpuOperationEvidenceKeys)
        || required_json_text(evidence, "schema")
            != "bioseq.persisted_gpu_operation_evidence"
        || required_json_counter(evidence, "version") != 2) {
        abort_pipeline_proof_capture(
            "persisted GPU operation evidence has the wrong exact schema");
    }

    const std::int64_t execution_id =
        required_json_counter(evidence, "execution_id");
    const std::int64_t backend_pid =
        required_json_counter(evidence, "backend_pid");
    const std::int64_t database_oid =
        required_json_counter(evidence, "database_oid");
    const bool completed = required_json_boolean(evidence, "completed");
    const bool partial = required_json_boolean(evidence, "partial");
    const bool fully_drained =
        required_json_boolean(evidence, "fully_drained");
    const bool producer_accepted =
        required_json_boolean(evidence, "accepted");
    const bool complete = required_json_boolean(evidence, "complete");
    const bool saturated = required_json_boolean(evidence, "saturated");
    const std::int64_t dropped_operation_count =
        required_json_counter(evidence, "dropped_operation_count");
    const std::int64_t operation_count =
        required_json_counter(evidence, "operation_count");
    const std::int64_t accepted_operation_count =
        required_json_counter(evidence, "accepted_operation_count");
    const std::int64_t complete_operation_count =
        required_json_counter(evidence, "complete_operation_count");
    const std::int64_t job_count =
        required_json_counter(evidence, "job_count");
    const std::int64_t accepted_job_count =
        required_json_counter(evidence, "accepted_job_count");
    const nlohmann::json& operations =
        required_json_array(evidence, "operations");
    const bool expects_persisted_gpu = out.actual_backend == "vulkan_gpu"
        && out.actual_target_index_source == "persisted";
    const auto qualification_found = evidence.find("cache_qualification");
    if (qualification_found == evidence.end()) {
        abort_pipeline_proof_capture(
            "persisted GPU cache qualification is absent");
    }

    if (execution_id != out.alignment_execution_id
        || backend_pid != out.alignment_backend_pid
        || database_oid != out.alignment_database_oid
        || completed != (out.alignment_completed != 0)
        || partial != (out.alignment_partial != 0)
        || fully_drained != (out.alignment_fully_drained != 0)) {
        abort_pipeline_proof_capture(
            "persisted GPU operation evidence identity/lifecycle differs from v2");
    }
    if (operations.size() > static_cast<std::size_t>(
            (std::numeric_limits<std::int64_t>::max)())
        || operation_count
            != static_cast<std::int64_t>(operations.size())) {
        abort_pipeline_proof_capture(
            "persisted GPU operation evidence operation count is inconsistent");
    }

    std::string qualification_identity;
    std::string qualification_seed_receipt_identity;
    std::string qualification_device_uuid;
    std::string qualification_pipeline_cache_uuid;
    std::string qualification_seed_sha256;
    std::string qualification_manifest_checksum;
    std::int64_t qualification_api_version = 0;
    std::int64_t qualification_driver_version = 0;
    std::int64_t qualification_vendor_id = 0;
    std::int64_t qualification_device_id = 0;
    std::int64_t qualification_seed_size = 0;
    bool cache_qualification_equations_valid = !expects_persisted_gpu;
    if (expects_persisted_gpu) {
        if (!qualification_found->is_object()
            || !json_has_exact_keys(
                *qualification_found,
                kPersistedGpuCacheQualificationKeys)) {
            abort_pipeline_proof_capture(
                "persisted GPU cache qualification has the wrong exact schema");
        }
        const nlohmann::json& qualification = *qualification_found;
        const std::int64_t qualification_version =
            required_json_counter(qualification, "version");
        qualification_identity = required_json_hex_identity(
            qualification, "identity", 16);
        const bool qualification_accepted =
            required_json_boolean(qualification, "accepted");
        const bool qualification_saturated =
            required_json_boolean(qualification, "saturated");
        const std::int64_t archive_count =
            required_json_counter(qualification, "archive_count");
        const std::int64_t archive_bytes =
            required_json_counter(qualification, "archive_bytes");
        (void)required_json_hex_identity(
            qualification, "archive_checksum", 16);
        const std::int64_t receipt_count =
            required_json_counter(qualification, "receipt_count");
        const std::int64_t hash_passes =
            required_json_counter(qualification, "hash_passes");
        const std::int64_t hashed_bytes =
            required_json_counter(qualification, "hashed_bytes");
        const std::int64_t private_files_created = required_json_counter(
            qualification, "private_files_created");
        const std::int64_t private_files_unlinked = required_json_counter(
            qualification, "private_files_unlinked");
        const std::int64_t live_private_paths = required_json_counter(
            qualification, "live_private_paths");
        const std::int64_t pipeline_filesystem_hash_passes =
            required_json_counter(
                qualification, "pipeline_filesystem_hash_passes");
        const std::int64_t pipeline_filesystem_hashed_bytes =
            required_json_counter(
                qualification, "pipeline_filesystem_hashed_bytes");
        const std::int64_t helper_protocol_version = required_json_counter(
            qualification, "helper_protocol_version");
        const std::string& helper_build_identity = required_json_text(
            qualification, "helper_build_identity");
        const std::string& helper_source =
            required_json_text(qualification, "helper_source");
        const std::int64_t helper_executable_size = required_json_counter(
            qualification, "helper_executable_size");
        (void)required_json_hex_identity(
            qualification, "helper_executable_sha256", 64);
        (void)required_json_hex_identity(
            qualification, "helper_metal_registry_id", 16);
        qualification_device_uuid = required_json_hex_identity(
            qualification, "device_uuid", 32);
        qualification_api_version =
            required_json_counter(qualification, "api_version");
        qualification_driver_version =
            required_json_counter(qualification, "driver_version");
        qualification_vendor_id =
            required_json_counter(qualification, "vendor_id");
        qualification_device_id =
            required_json_counter(qualification, "device_id");
        qualification_pipeline_cache_uuid = required_json_hex_identity(
            qualification, "pipeline_cache_uuid", 32);
        qualification_seed_receipt_identity = required_json_hex_identity(
            qualification, "seed_receipt_identity", 16);
        qualification_seed_sha256 = required_json_hex_identity(
            qualification, "seed_sha256", 64);
        qualification_seed_size =
            required_json_counter(qualification, "seed_size");
        qualification_manifest_checksum = required_json_hex_identity(
            qualification, "manifest_checksum", 16);

        const bool helper_build_valid = helper_build_identity
            == "bioseq_gpu_archive_builder_v2";
        const bool numeric_driver_identity_valid =
            qualification_api_version > 0
            && qualification_driver_version > 0
            && qualification_api_version
                <= (std::numeric_limits<std::uint32_t>::max)()
            && qualification_driver_version
                <= (std::numeric_limits<std::uint32_t>::max)()
            && qualification_vendor_id
                <= (std::numeric_limits<std::uint32_t>::max)()
            && qualification_device_id
                <= (std::numeric_limits<std::uint32_t>::max)();
        const std::string canonical_driver = numeric_driver_identity_valid
            ? fmt::format(
                "api={:08x};driver={:08x};vendor={:08x};device={:08x};pcuuid={}",
                static_cast<std::uint32_t>(qualification_api_version),
                static_cast<std::uint32_t>(qualification_driver_version),
                static_cast<std::uint32_t>(qualification_vendor_id),
                static_cast<std::uint32_t>(qualification_device_id),
                qualification_pipeline_cache_uuid)
            : std::string{};
        cache_qualification_equations_valid = qualification_version == 2
            && qualification_accepted && !qualification_saturated
            && archive_count > 0 && archive_bytes > 0
            && receipt_count == archive_count && hash_passes == 1
            && hashed_bytes == archive_bytes
            && private_files_created == archive_count
            && private_files_unlinked == archive_count
            && live_private_paths == 0
            && pipeline_filesystem_hash_passes == 0
            && pipeline_filesystem_hashed_bytes == 0
            && helper_protocol_version == 2 && helper_build_valid
            && helper_source == "installed-sibling"
            && helper_executable_size > 0
            && numeric_driver_identity_valid
            && qualification_device_uuid == out.gpu_device_uuid
            && canonical_driver == out.gpu_driver
            && qualification_seed_sha256
                == out.gpu_pipeline_cache_seed_sha256
            && qualification_seed_size == out.gpu_pipeline_cache_seed_size
            && qualification_manifest_checksum
                == out.gpu_pipeline_cache_manifest_checksum;
    } else if (!qualification_found->is_null()) {
        abort_pipeline_proof_capture(
            "non-persisted-GPU evidence retained cache qualification");
    }

    bool operation_equations_valid = true;
    __int128 expected_job_sum = 0;
    __int128 accepted_job_sum = 0;
    __int128 cache_load_sum = 0;
    __int128 cache_seed_bytes_sum = 0;
    __int128 cache_load_ns_sum = 0;
    __int128 pipeline_create_sum = 0;
    __int128 pipeline_creation_ns_sum = 0;
    std::set<std::string> operation_identities;
    std::set<std::string> pipeline_operation_identities;
    std::set<std::tuple<std::string, std::int64_t, std::int64_t>>
        logical_jobs;
    std::optional<std::string> common_seed_receipt_identity;
    std::optional<std::string> common_cold_load_identity;
    bool output_coordinate_witness = false;

    for (const nlohmann::json& operation : operations) {
        if (!json_has_exact_keys(operation, kPersistedGpuOperationKeys)) {
            abort_pipeline_proof_capture(
                "persisted GPU operation has the wrong exact schema");
        }
        const std::string& operation_identity = required_json_hex_identity(
            operation, "operation_identity", 16);
        const std::int64_t expected_jobs =
            required_json_counter(operation, "expected_jobs");
        const std::int64_t proof_jobs =
            required_json_counter(operation, "proof_jobs");
        const std::int64_t consumed_jobs =
            required_json_counter(operation, "consumed_jobs");
        const std::int64_t accepted_jobs =
            required_json_counter(operation, "accepted_jobs");
        const std::int64_t burned_jobs =
            required_json_counter(operation, "burned_jobs");
        const std::int64_t pending_jobs =
            required_json_counter(operation, "pending_jobs");
        const std::int64_t staged_jobs =
            required_json_counter(operation, "staged_jobs");
        const std::int64_t published_jobs =
            required_json_counter(operation, "published_jobs");
        const std::int64_t publication_commits =
            required_json_counter(operation, "publication_commits");
        const std::int64_t query_receipts = required_json_counter(
            operation, "query_pipeline_receipt_count");
        const std::int64_t shape_receipts = required_json_counter(
            operation, "shape_pipeline_receipt_count");
        const std::int64_t submission_count =
            required_json_counter(operation, "submission_count");
        const std::string& query_operation_identity =
            required_json_hex_identity(
                operation, "query_pipeline_operation_identity", 16);
        const std::string& shape_operation_identity =
            required_json_hex_identity(
                operation, "shape_pipeline_operation_identity", 16);
        const nlohmann::json& query_submission_ordinals =
            required_json_array(operation, "query_submission_ordinals");
        const std::string& seed_receipt_identity =
            required_json_hex_identity(
                operation, "seed_receipt_identity", 16);
        const std::string& cache_token_identity =
            required_json_hex_identity(
                operation, "cache_token_identity", 16);
        const std::string& cache_qualification_identity =
            required_json_hex_identity(
                operation, "cache_qualification_identity", 16);
        const std::int64_t generation_id =
            required_json_counter(operation, "generation_id");
        const std::string& source_identity = required_json_hex_identity(
            operation, "source_identity", 16);
        const std::string& cold_load_identity = required_json_hex_identity(
            operation, "cold_load_identity", 16);
        const std::string& source = required_json_text(operation, "source");
        const nlohmann::json& environment =
            required_json_object(operation, "environment");
        const nlohmann::json& cache_metrics =
            required_json_object(operation, "cache_metrics");
        const nlohmann::json& jobs = required_json_array(operation, "jobs");

        if (!json_has_exact_keys(environment, kPersistedGpuEnvironmentKeys)
            || !json_has_exact_keys(
                cache_metrics, kPersistedGpuCacheMetricsKeys)) {
            abort_pipeline_proof_capture(
                "persisted GPU operation environment/cache schema is invalid");
        }
        const std::string& device_uuid = required_json_hex_identity(
            environment, "device_uuid", 32);
        const std::int64_t api_version =
            required_json_counter(environment, "api_version");
        const std::int64_t driver_version =
            required_json_counter(environment, "driver_version");
        const std::int64_t vendor_id =
            required_json_counter(environment, "vendor_id");
        const std::int64_t device_id =
            required_json_counter(environment, "device_id");
        const std::string& pipeline_cache_uuid =
            required_json_hex_identity(
                environment, "pipeline_cache_uuid", 32);
        const std::string& seed_sha256 = required_json_hex_identity(
            environment, "seed_sha256", 64);
        const std::int64_t seed_size =
            required_json_counter(environment, "seed_size");
        const std::string& manifest_checksum = required_json_hex_identity(
            environment, "manifest_checksum", 16);

        const bool numeric_driver_identity_valid = api_version > 0
            && driver_version > 0
            && api_version <= (std::numeric_limits<std::uint32_t>::max)()
            && driver_version <= (std::numeric_limits<std::uint32_t>::max)()
            && vendor_id <= (std::numeric_limits<std::uint32_t>::max)()
            && device_id <= (std::numeric_limits<std::uint32_t>::max)();
        const std::string canonical_driver = numeric_driver_identity_valid
            ? fmt::format(
                "api={:08x};driver={:08x};vendor={:08x};device={:08x};pcuuid={}",
                static_cast<std::uint32_t>(api_version),
                static_cast<std::uint32_t>(driver_version),
                static_cast<std::uint32_t>(vendor_id),
                static_cast<std::uint32_t>(device_id),
                pipeline_cache_uuid)
            : std::string{};
        const bool environment_valid = numeric_driver_identity_valid
            && device_uuid == out.gpu_device_uuid
            && canonical_driver == out.gpu_driver
            && seed_sha256 == out.gpu_pipeline_cache_seed_sha256
            && seed_size == out.gpu_pipeline_cache_seed_size
            && manifest_checksum == out.gpu_pipeline_cache_manifest_checksum;

        const std::int64_t cache_loads = required_json_counter(
            cache_metrics, "pipeline_cache_loads");
        const std::int64_t cache_seed_bytes = required_json_counter(
            cache_metrics, "pipeline_cache_seed_bytes");
        const std::int64_t cache_load_ns = required_json_counter(
            cache_metrics, "pipeline_cache_load_ns");
        const std::int64_t pipeline_creates = required_json_counter(
            cache_metrics, "pipeline_creates");
        const std::int64_t pipeline_creation_ns = required_json_counter(
            cache_metrics, "pipeline_creation_ns");
        const bool cache_shape_valid = cache_loads <= 1
            && (cache_loads == 0
                ? cache_seed_bytes == 0 && cache_load_ns == 0
                : cache_seed_bytes == seed_size && cache_load_ns > 0)
            && static_cast<__int128>(pipeline_creates)
                <= static_cast<__int128>(shape_receipts) + 1
            && (pipeline_creates == 0
                ? pipeline_creation_ns == 0
                : pipeline_creation_ns > 0);
        cache_load_sum += cache_loads;
        cache_seed_bytes_sum += cache_seed_bytes;
        cache_load_ns_sum += cache_load_ns;
        pipeline_create_sum += pipeline_creates;
        pipeline_creation_ns_sum += pipeline_creation_ns;

        std::set<std::pair<std::string, std::int64_t>>
            structured_submissions;
        std::map<
            std::pair<std::string, std::int64_t>,
            std::pair<std::size_t, std::string>> shape_submission_receipts;
        std::map<
            std::string,
            std::pair<std::string, std::int64_t>> submit_wait_domains;
        bool query_submissions_valid = query_submission_ordinals.size() == 2;
        for (const nlohmann::json& ordinal_json : query_submission_ordinals) {
            const std::int64_t ordinal =
                required_json_integer_value(ordinal_json);
            query_submissions_valid = query_submissions_valid && ordinal > 0
                && structured_submissions.emplace(
                    query_operation_identity, ordinal).second;
        }

        bool jobs_valid = jobs.size() <= static_cast<std::size_t>(
                (std::numeric_limits<std::int64_t>::max)())
            && expected_jobs == static_cast<std::int64_t>(jobs.size());
        constexpr std::array<std::string_view, 4> expected_submission_stages{
            "count", "emit", "sort_range", "chain_coordinate",
        };
        for (const nlohmann::json& job : jobs) {
            if (!json_has_exact_keys(job, kPersistedGpuJobKeys)) {
                abort_pipeline_proof_capture(
                    "persisted GPU job has the wrong exact schema");
            }
            const std::int64_t logical_target_row =
                required_json_counter(job, "logical_target_row");
            const std::int64_t logical_query_ordinal =
                required_json_counter(job, "logical_query_ordinal");
            const std::int64_t query_index =
                required_json_counter(job, "query_index");
            const std::int64_t query_length =
                required_json_counter(job, "query_length");
            const std::int64_t query_minimizer_count =
                required_json_counter(job, "query_minimizer_count");
            const std::int64_t query_tile_count =
                required_json_counter(job, "query_tile_count");
            const nlohmann::json& query =
                required_json_object(job, "query");
            const nlohmann::json& direct =
                required_json_object(job, "direct");
            const nlohmann::json& submissions =
                required_json_array(job, "submissions");
            const nlohmann::json& status =
                required_json_object(job, "status");
            const nlohmann::json& forbidden =
                required_json_object(job, "forbidden");
            const nlohmann::json& coordinate =
                required_json_object(job, "coordinate");
            if (!json_has_exact_keys(query, kPersistedGpuQueryKeys)
                || !json_has_exact_keys(direct, kPersistedGpuDirectKeys)
                || !json_has_exact_keys(status, kPersistedGpuStatusKeys)
                || !json_has_exact_keys(
                    forbidden, kPersistedGpuForbiddenKeys)
                || !json_has_exact_keys(
                    coordinate, kPersistedGpuCoordinateKeys)) {
                abort_pipeline_proof_capture(
                    "persisted GPU job nested object schema is invalid");
            }

            const bool query_valid = query_length > 0
                && query_minimizer_count > 0 && query_tile_count > 0
                && required_json_counter(query, "count_dispatches") == 1
                && required_json_counter(query, "scan_dispatches") == 3
                && required_json_counter(query, "emit_dispatches") == 1
                && required_json_counter(query, "completed_roundtrips") == 2;
            const std::int64_t anchor_count =
                required_json_counter(direct, "anchor_count");
            const std::int64_t range_count =
                required_json_counter(direct, "range_count");
            const bool direct_valid =
                required_json_counter(direct, "count_dispatches") == 1
                && required_json_counter(direct, "scan_dispatches") == 3
                && anchor_count > 0
                && required_json_counter(direct, "emit_dispatches") == 1
                && required_json_counter(direct, "sort_operations") == 1
                && required_json_counter(
                    direct, "range_build_dispatches") == 1
                && range_count > 0
                && required_json_counter(direct, "chain_dispatches") == 1
                && required_json_counter(
                    direct, "chain_backtrack_dispatches") == 1
                && required_json_counter(
                    direct, "coordinate_dispatches") == 1;

            bool submissions_valid =
                submissions.size() == expected_submission_stages.size();
            std::set<std::int64_t> job_submission_ordinals;
            std::set<std::string> job_submit_wait_identities;
            for (std::size_t submission_index = 0;
                 submission_index < submissions.size(); ++submission_index) {
                const nlohmann::json& submission =
                    submissions[submission_index];
                if (!json_has_exact_keys(
                        submission, kPersistedGpuSubmissionKeys)) {
                    abort_pipeline_proof_capture(
                        "persisted GPU submission has the wrong exact schema");
                }
                const std::string& stage =
                    required_json_text(submission, "stage");
                const std::string& submission_operation_identity =
                    required_json_hex_identity(
                        submission, "operation_identity", 16);
                const std::int64_t ordinal =
                    required_json_counter(submission, "ordinal");
                const std::string& submit_wait_identity =
                    required_json_hex_identity(
                        submission, "submit_wait_identity", 16);
                const auto submission_domain = std::pair{
                    submission_operation_identity, ordinal};
                const auto [receipt, receipt_inserted] =
                    shape_submission_receipts.emplace(
                        submission_domain,
                        std::pair{submission_index, submit_wait_identity});
                const bool receipt_consistent = receipt_inserted
                    || receipt->second
                        == std::pair{submission_index, submit_wait_identity};
                const auto [wait_domain, wait_inserted] =
                    submit_wait_domains.emplace(
                        submit_wait_identity, submission_domain);
                const bool wait_consistent = wait_inserted
                    || wait_domain->second == submission_domain;
                submissions_valid = submissions_valid
                    && submission_index < expected_submission_stages.size()
                    && stage == expected_submission_stages[submission_index]
                    && submission_operation_identity
                        == shape_operation_identity
                    && ordinal > 0
                    && job_submission_ordinals.emplace(ordinal).second
                    && job_submit_wait_identities.emplace(
                        submit_wait_identity).second
                    && receipt_consistent && wait_consistent;
                structured_submissions.emplace(
                    submission_operation_identity, ordinal);
            }

            const bool status_valid =
                required_json_counter(status, "count_checks") == 1
                && required_json_counter(status, "emit_checks") == 1
                && required_json_counter(status, "status_or") == 0
                && required_json_counter(status, "result_readbacks") == 1;
            const bool forbidden_valid = required_json_counter(
                    forbidden, "coordinate_vote_dispatches") == 0
                && required_json_counter(
                    forbidden, "cpu_or_simd_biological_calls") == 0
                && required_json_counter(forbidden, "recompute_calls") == 0
                && required_json_counter(forbidden, "fallback_calls") == 0;

            const bool coordinate_valid_flag =
                required_json_boolean(coordinate, "valid");
            const std::int64_t query_start =
                required_json_counter(coordinate, "query_start");
            const std::int64_t query_end =
                required_json_counter(coordinate, "query_end");
            const std::int64_t target_start =
                required_json_counter(coordinate, "target_start");
            const std::int64_t target_end =
                required_json_counter(coordinate, "target_end");
            const std::int64_t match_length =
                required_json_counter(coordinate, "match_length");
            const std::int64_t block_length =
                required_json_counter(coordinate, "block_length");
            const std::int64_t chain_score =
                required_json_counter(coordinate, "chain_score");
            const std::int64_t seed_count =
                required_json_counter(coordinate, "seed_count");
            const bool is_reverse =
                required_json_boolean(coordinate, "is_reverse");
            const bool coordinate_valid = coordinate_valid_flag
                && query_start < query_end && query_end <= query_length
                && target_start < target_end
                && match_length > 0 && match_length <= block_length
                && match_length <= query_end - query_start
                && match_length <= target_end - target_start
                && static_cast<__int128>(block_length)
                    <= static_cast<__int128>(query_end - query_start)
                        + static_cast<__int128>(target_end - target_start)
                && chain_score > 0 && seed_count > 0;

            if (query_index == 0
                && target_start == out.target_start
                && is_reverse == out.is_reverse) {
                output_coordinate_witness = true;
            }

            jobs_valid = jobs_valid
                && logical_jobs.emplace(
                    operation_identity,
                    logical_target_row,
                    logical_query_ordinal).second
                && query_index < expected_jobs
                && query_valid && direct_valid && submissions_valid
                && status_valid && forbidden_valid && coordinate_valid;
        }

        const bool seed_receipt_matches =
            !common_seed_receipt_identity.has_value()
                || *common_seed_receipt_identity == seed_receipt_identity;
        const bool cold_load_matches = !common_cold_load_identity.has_value()
            || *common_cold_load_identity == cold_load_identity;
        if (!common_seed_receipt_identity.has_value()) {
            common_seed_receipt_identity = seed_receipt_identity;
        }
        if (!common_cold_load_identity.has_value()) {
            common_cold_load_identity = cold_load_identity;
        }
        const bool identity_valid =
            operation_identities.emplace(operation_identity).second
            && pipeline_operation_identities.emplace(
                query_operation_identity).second
            && pipeline_operation_identities.emplace(
                shape_operation_identity).second
            && operation_identity != query_operation_identity
            && operation_identity == shape_operation_identity
            && query_operation_identity != shape_operation_identity
            && cache_token_identity == operation_identity
            && cache_qualification_identity == qualification_identity
            && seed_receipt_identity
                == qualification_seed_receipt_identity
            && seed_receipt_matches && cold_load_matches
            && generation_id == out.persisted_generation_id
            && source_identity == out.persisted_source_identity
            && source == "persisted_shape_v5"
            && device_uuid == qualification_device_uuid
            && api_version == qualification_api_version
            && driver_version == qualification_driver_version
            && vendor_id == qualification_vendor_id
            && device_id == qualification_device_id
            && pipeline_cache_uuid == qualification_pipeline_cache_uuid
            && seed_sha256 == qualification_seed_sha256
            && seed_size == qualification_seed_size
            && manifest_checksum == qualification_manifest_checksum;
        const bool publication_valid = expected_jobs > 0
            && proof_jobs == expected_jobs
            && consumed_jobs == expected_jobs
            && accepted_jobs == expected_jobs
            && burned_jobs == 0 && pending_jobs == 0
            && staged_jobs == expected_jobs
            && published_jobs == expected_jobs
            && publication_commits == 1
            && query_receipts == 3
            && shape_receipts >= 7 && shape_receipts <= 10
            && query_submissions_valid
            && submission_count > 0
            && structured_submissions.size() <= static_cast<std::size_t>(
                (std::numeric_limits<std::int64_t>::max)())
            && submission_count
                == static_cast<std::int64_t>(structured_submissions.size());
        operation_equations_valid = operation_equations_valid
            && identity_valid && publication_valid && environment_valid
            && cache_shape_valid && jobs_valid;
        expected_job_sum += expected_jobs;
        accepted_job_sum += accepted_jobs;
    }

    const auto counter = [&out](const std::size_t column) noexcept {
        return out.alignment_snapshot_numeric_columns[column];
    };
    const bool aggregate_cache_valid =
        cache_load_sum == static_cast<__int128>(counter(124))
        && cache_seed_bytes_sum == static_cast<__int128>(counter(125))
        && cache_load_ns_sum == static_cast<__int128>(counter(126))
        && pipeline_creation_ns_sum == static_cast<__int128>(counter(127))
        && pipeline_create_sum == static_cast<__int128>(counter(138));
    const bool accepted_equations = producer_accepted
        && completed && !partial && fully_drained && complete && !saturated
        && dropped_operation_count == 0 && operation_count > 0
        && accepted_operation_count == operation_count
        && complete_operation_count == operation_count
        && expected_job_sum == static_cast<__int128>(job_count)
        && accepted_job_sum == static_cast<__int128>(accepted_job_count)
        && job_count == out.attempted_alignment_jobs
        && accepted_job_count == out.fully_attested_alignment_jobs
        && out.unattested_alignment_jobs == 0
        && output_coordinate_witness
        && cache_qualification_equations_valid
        && operation_equations_valid && aggregate_cache_valid;
    const bool exact_empty_non_gpu_evidence = !producer_accepted && complete
        && !saturated && dropped_operation_count == 0
        && operation_count == 0 && accepted_operation_count == 0
        && complete_operation_count == 0 && job_count == 0
        && accepted_job_count == 0 && operations.empty();
    if (!expects_persisted_gpu && !exact_empty_non_gpu_evidence) {
        abort_pipeline_proof_capture(
            "non-persisted-GPU snapshot retained stale operation evidence");
    }

    out.persisted_gpu_operation_evidence_schema_valid = 1;
    out.persisted_gpu_operation_evidence_accepted =
        expects_persisted_gpu && accepted_equations ? 1 : 0;
    out.persisted_gpu_operation_evidence_complete = complete ? 1 : 0;
    out.persisted_gpu_operation_evidence_saturated = saturated ? 1 : 0;
    out.persisted_gpu_dropped_operation_count = dropped_operation_count;
    out.persisted_gpu_operation_count = operation_count;
    out.persisted_gpu_accepted_operation_count = accepted_operation_count;
    out.persisted_gpu_job_count = job_count;
    out.persisted_gpu_accepted_job_count = accepted_job_count;
    out.persisted_gpu_operation_evidence = evidence;
}

void capture_alignment_statement_stats(
        const it::connection_test_context& connection,
        const AlignmentSnapshotIdentity& prior,
        E2eOutput& out) {
    constexpr std::size_t kColumnCount =
        E2eOutput::kAlignmentSnapshotColumnCount;
    constexpr std::size_t kStageCount = 20;
    const std::array<std::string_view, kStageCount> expected_stages{
        "query_sketch", "target_sketch", "target_index_build",
        "persisted_index_decode", "partition_selection",
        "device_index_upload", "seed_lookup", "chain_score",
        "chain_backtrack", "region_generation",
        "primary_secondary_reduction", "divergence_estimation", "mapq",
        "target_window_plan", "target_window_io", "target_window_upload",
        "cigar_job_generation", "left_extension", "gap_extension",
        "right_extension",
    };
    const it::rowset_t rows = it::query_rows(
        connection,
        "SELECT * FROM bioseq.last_alignment_execution_stats_v2();");
    // This must be the very next SQL statement on the same backend. Retain
    // its scalar value now, before any other diagnostic query can obscure the
    // one-shot cache-qualified ledger bound to the execution identity.
    const it::row_t persisted_gpu_operation_row = it::query_one(
        connection,
        "SELECT bioseq.last_alignment_persisted_gpu_operation_evidence_v2();");
    if (rows.size() != kStageCount) {
        abort_pipeline_proof_capture(
            "alignment snapshot did not return exactly 20 stage rows");
    }
    for (std::size_t row_no = 0; row_no < rows.size(); ++row_no) {
        const auto& row = rows[row_no];
        if (row.size() != kColumnCount
            || !row[23].has_value()
            || *row[23] != expected_stages[row_no]) {
            abort_pipeline_proof_capture(
                "alignment snapshot has an invalid column/stage shape");
        }
        for (std::size_t column = 0; column < row.size(); ++column) {
            if (!row[column].has_value()
                || alignment_stats_text_column(column)
                || alignment_stats_json_column(column)) {
                continue;
            }
            if (alignment_stats_boolean_column(column)) {
                if (!parse_snapshot_bool(row[column]).has_value()) {
                    abort_pipeline_proof_capture(
                        "alignment snapshot contains an ill-typed Boolean");
                }
            } else if (!parse_snapshot_i64(row[column]).has_value()) {
                abort_pipeline_proof_capture(
                    "alignment snapshot contains an ill-typed integer");
            }
        }
        if (row_no != 0) {
            for (std::size_t column = 0; column < row.size(); ++column) {
                const bool stage_local = column >= 23 && column <= 31;
                if (!stage_local && row[column] != rows.front()[column]) {
                    abort_pipeline_proof_capture(
                        "alignment snapshot envelope differs between stage rows");
                }
            }
        }
    }
    // Retain the complete, already shape/type-validated SRF value.  This is a
    // copy made after the timed statement; it cannot affect the measured
    // region and it is the sole source for raw attestation serialization.
    out.alignment_snapshot_rows = rows;
    capture_versioned_alignment_json(rows, out);

    const auto required_i64 = [&rows](const std::size_t column) {
        const auto parsed = parse_snapshot_i64(rows.front()[column]);
        if (!parsed.has_value()) {
            abort_pipeline_proof_capture(
                "alignment snapshot required integer is NULL/invalid");
        }
        return *parsed;
    };
    const auto required_bool = [&rows](const std::size_t column) {
        const auto parsed = parse_snapshot_bool(rows.front()[column]);
        if (!parsed.has_value()) {
            abort_pipeline_proof_capture(
                "alignment snapshot required Boolean is NULL/invalid");
        }
        return *parsed;
    };
    for (std::size_t column = 0; column < kColumnCount; ++column) {
        if (alignment_stats_text_column(column)
            || alignment_stats_json_column(column)
            || !rows.front()[column].has_value()) {
            continue;
        }
        const auto parsed = alignment_stats_boolean_column(column)
            ? parse_snapshot_bool(rows.front()[column])
            : parse_snapshot_i64(rows.front()[column]);
        if (!parsed.has_value()) {
            abort_pipeline_proof_capture(
                "alignment snapshot numeric envelope is ill-typed");
        }
        out.alignment_snapshot_numeric_columns[column] = *parsed;
    }
    const std::int64_t execution_id = required_i64(0);
    const std::int64_t backend_pid = required_i64(1);
    const std::int64_t database_oid = required_i64(2);
    if (prior.execution_id == (std::numeric_limits<std::int64_t>::max)()
        || execution_id != prior.execution_id + 1
        || backend_pid != prior.backend_pid
        || database_oid != prior.database_oid) {
        abort_pipeline_proof_capture(
            "alignment snapshot is stale or belongs to another backend/database");
    }

    out.alignment_execution_id = execution_id;
    out.prior_alignment_execution_id = prior.execution_id;
    out.alignment_backend_pid = backend_pid;
    out.alignment_database_oid = database_oid;
    out.timed_postmaster_start_time = prior.postmaster_start_time;
    out.effective_cpu_workers = rows.front()[3].has_value()
        ? required_i64(3) : -1;
    out.gpu_device_uuid = rows.front()[4].value_or("");
    out.gpu_driver = rows.front()[5].value_or("");
    out.gpu_pipeline_cache_policy = rows.front()[6].value_or("");
    out.gpu_pipeline_cache_seed_sha256 = rows.front()[7].value_or("");
    out.gpu_pipeline_cache_seed_size = rows.front()[8].has_value()
        ? required_i64(8) : -1;
    out.gpu_pipeline_cache_manifest_checksum =
        rows.front()[9].value_or("");
    out.gpu_pipeline_cache_hit = rows.front()[10].has_value()
        ? (required_bool(10) ? 1 : 0) : -1;
    out.alignment_scan_node_count = required_i64(11);
    out.nested_alignment_executor_count = required_i64(12);
    out.alignment_completed = required_bool(13);
    out.alignment_partial = required_bool(14);
    out.alignment_fully_drained = required_bool(15);
    out.requested_backend = rows.front()[16].value_or("");
    out.actual_backend = rows.front()[17].value_or("");
    out.requested_target_index_source = rows.front()[18].value_or("");
    out.actual_target_index_source = rows.front()[19].value_or("");
    out.persisted_generation_id = required_i64(20);
    out.persisted_source_identity = rows.front()[21].value_or("");
    out.mixed_persisted_identity = required_bool(22);
    out.target_rows = required_i64(32);
    out.attempted_alignment_jobs = required_i64(33);
    out.fully_attested_alignment_jobs = required_i64(34);
    out.unattested_alignment_jobs = required_i64(35);
    out.first_unattested_target_row = rows.front()[36].has_value()
        ? required_i64(36) : -1;
    out.first_unattested_query_ordinal = rows.front()[37].has_value()
        ? required_i64(37) : -1;
    out.cpu_prepared_index_builds = required_i64(38);
    out.cpu_prepared_index_reuses = required_i64(39);
    out.cpu_prepared_unique_keys = required_i64(40);
    out.cpu_prepared_duplicate_build_attempts = required_i64(41);
    out.cpu_prepared_key_set_checksum = rows.front()[42].value_or("");
    out.cpu_prepared_build_set_checksum = rows.front()[43].value_or("");
    out.operational_identity_conflict = required_bool(44);
    out.persisted_index_rows = required_i64(45);
    out.missing_persisted_index_rows = required_i64(46);
    out.recomputed_target_index_rows = required_i64(47);
    out.persisted_generation_read_sessions = required_i64(48);
    out.persisted_relation_opens = required_i64(49);
    out.persisted_index_scans = required_i64(50);
    out.persisted_manifest_rows_read = required_i64(55);
    out.persisted_manifest_bytes_read = required_i64(56);
    out.host_query_hash_readbacks = required_i64(87);
    out.host_query_symbol_packing_calls = required_i64(88);
    out.host_target_symbol_packing_calls = required_i64(89);
    out.host_alignment_job_classification_calls = required_i64(90);
    out.host_cigar_construction_calls = required_i64(92);
    out.gpu_persisted_resident_uploads = required_i64(110);
    out.gpu_persisted_resident_cache_hits = required_i64(111);
    out.gpu_seed_dispatches = required_i64(113);
    out.gpu_chain_dispatches = required_i64(114);
    out.gpu_chain_backtrack_dispatches = required_i64(115);
    out.gpu_extend_dispatches = required_i64(116);
    out.backend_declines = required_i64(139);
    out.backend_fallbacks = required_i64(140);
    out.stats_saturated = required_bool(141);
    capture_persisted_gpu_operation_evidence(
        persisted_gpu_operation_row, out);
    capture_format5_reader_memory_evidence(connection, out);
    capture_format6_reader_evidence(connection, out);
    out.query_sketch_host_wall_ns = required_i64(30);
    out.target_index_build_host_wall_ns = [&rows] {
        const auto parsed = parse_snapshot_i64(rows[2][30]);
        if (!parsed.has_value()) {
            abort_pipeline_proof_capture(
                "target-index stage timing is NULL/invalid");
        }
        return *parsed;
    }();

    const bool non_gpu_environment =
        !rows.front()[4].has_value() && !rows.front()[5].has_value()
        && !rows.front()[6].has_value() && !rows.front()[7].has_value()
        && !rows.front()[8].has_value() && !rows.front()[9].has_value()
        && !rows.front()[10].has_value();
    const bool gpu_environment = rows.front()[4].has_value()
        && fixed_lower_hex(*rows.front()[4], 32)
        && rows.front()[5].has_value() && !rows.front()[5]->empty()
        && rows.front()[6].has_value()
        && rows.front()[7].has_value()
        && fixed_lower_hex(*rows.front()[7], 64)
        && rows.front()[8].has_value()
        && rows.front()[9].has_value()
        && fixed_lower_hex(*rows.front()[9], 16)
        && rows.front()[10].has_value();
    const bool environment_valid = out.effective_cpu_workers > 0
        && ((out.actual_backend == "vulkan_gpu" && gpu_environment)
            || (out.actual_backend != "vulkan_gpu" && non_gpu_environment));
    out.environment_mixed_invalid = environment_valid ? 0 : 1;
    out.pipeline_proof_present = 1;
    out.pipeline_proof_attached_to_measurement = 1;
}

// Parse one exact primary PAF row per query. Every accepted row must carry a
// cg:Z CIGAR which replays over the reported half-open query/target spans.
void parse_minimap2_paf(const std::string& paf, const E2eInput& in, E2eOutput& out) {
    auto query_id = [](std::string_view qname) -> std::int64_t {
        constexpr std::string_view prefix = "query";
        if (!qname.starts_with(prefix)) return -1;
        const auto parsed = parse_i64_exact(qname.substr(prefix.size()));
        return parsed.value_or(-1);
    };
    std::vector<std::uint8_t> primary_counts(in.queries.size(), 0);
    std::vector<double> offsets(in.queries.size(), kNoMatchOffsetSentinel);
    std::int64_t primaries = 0;
    std::int64_t records = 0;
    bool malformed_or_extra = false;
    std::size_t pos = 0;
    while (pos < paf.size()) {
        const std::size_t eol = paf.find('\n', pos);
        const std::string_view line{paf.data() + pos, (eol == std::string::npos ? paf.size() : eol) - pos};
        pos = (eol == std::string::npos) ? paf.size() : eol + 1;
        if (line.empty()) continue;
        ++records;
        std::vector<std::string_view> col; col.reserve(18);
        std::size_t cs = 0;
        for (std::size_t i = 0; i <= line.size(); ++i)
            if (i == line.size() || line[i] == '\t') { col.emplace_back(line.substr(cs, i - cs)); cs = i + 1; }
        if (col.size() < 12) { malformed_or_extra = true; continue; }
        const std::int64_t qid64 = query_id(col[0]);
        if (qid64 < 0 || static_cast<std::size_t>(qid64) >= in.queries.size()) {
            malformed_or_extra = true;
            continue;
        }
        const auto qid = static_cast<std::size_t>(qid64);
        const auto query_length = parse_i64_exact(col[1]);
        const auto query_start = parse_i64_exact(col[2]);
        const auto query_end = parse_i64_exact(col[3]);
        const std::string_view rname = col[5];
        const auto rec_idx = rname.starts_with("ref")
            ? parse_i64_exact(rname.substr(3)) : std::nullopt;
        const auto target_start = parse_i64_exact(col[7]);
        const auto target_end = parse_i64_exact(col[8]);
        if (!query_length.has_value() || !query_start.has_value()
            || !query_end.has_value() || !rec_idx.has_value()
            || !target_start.has_value() || !target_end.has_value()
            || *query_length != static_cast<std::int64_t>(in.queries[qid].bases.size())
            || *query_start < 0 || *query_end < *query_start
            || *query_end > *query_length || *rec_idx < 0
            || *target_start < 0 || *target_end < *target_start
            || (col[4] != "+" && col[4] != "-")) {
            malformed_or_extra = true;
            continue;
        }
        bool is_primary = false;
        std::optional<std::string_view> cigar;
        for (std::size_t tag = 12; tag < col.size(); ++tag) {
            if (col[tag] == "tp:A:P") is_primary = true;
            if (col[tag].starts_with("cg:Z:")) {
                if (cigar.has_value()) malformed_or_extra = true;
                cigar = col[tag].substr(5);
            }
        }
        if (!is_primary || !cigar.has_value()) {
            malformed_or_extra = true;
            continue;
        }
        ++primaries;
        ++primary_counts[qid];
        if (primary_counts[qid] != 1) malformed_or_extra = true;
        const std::int64_t abs_start =
            *rec_idx * static_cast<std::int64_t>(in.minimap2_split_bp)
                + *target_start;
        const std::int64_t abs_end =
            *rec_idx * static_cast<std::int64_t>(in.minimap2_split_bp)
                + *target_end;
        const bool rev = (col[4] == "-");
        const double offset = static_cast<double>(std::llabs(
            abs_start - in.queries[qid].truth_target_start));
        offsets[qid] = offset;
        ++out.strand_result_count;
        if (rev == in.queries[qid].truth_is_reverse) {
            ++out.correct_strand_count;
        }
        if (is_valid_benchmark_cigar(*cigar)) ++out.cigar_result_count;
        if (offset < static_cast<double>(kPositionToleranceBp)
            && rev == in.queries[qid].truth_is_reverse
            && is_expected_benchmark_cigar(*cigar, in)
            && replay_benchmark_cigar(
                *cigar, in, in.queries[qid], *query_start, *query_end,
                abs_start, abs_end, rev)) {
            ++out.correct_cigar_count;
            ++out.cigar_replay_count;
        }
        if (qid == 0) {
            out.target_start = abs_start;
            out.is_reverse = rev;
        }
    }

    out.expected_match_count = static_cast<std::int64_t>(in.queries.size());
    out.match_count = primaries;
    out.correct_match_count = 0;
    out.max_abs_offset_bp = 0.0;
    for (std::size_t i = 0; i < in.queries.size(); ++i) {
        const double offset = offsets[i];
        out.max_abs_offset_bp = std::max(out.max_abs_offset_bp, offset);
        if (primary_counts[i] == 1
            && offset < static_cast<double>(kPositionToleranceBp)) {
            ++out.correct_match_count;
        }
    }
    out.exact_primary_count_accepted = !malformed_or_extra
        && records == out.expected_match_count
        && std::ranges::all_of(primary_counts,
            [](const std::uint8_t count) noexcept { return count == 1; })
        ? 1 : 0;
    out.found = out.exact_primary_count_accepted == 1
        && out.correct_match_count == out.expected_match_count
        && out.correct_strand_count == out.expected_match_count
        && (!in.cigar_projection
            || (out.correct_cigar_count == out.expected_match_count
                && out.cigar_replay_count == out.expected_match_count));
    out.abs_offset_bp = out.max_abs_offset_bp;
}

// HIGH-09/TESTING-04: the ground truth is known, so any miss or mis-map is ALWAYS
// a bug. Fail loudly rather than letting a fast, meaningless timing be reported
// as a "win". Called AFTER the timed region.
inline void require_mapped(std::string_view algo, const E2eInput& in,
                           const E2eOutput& out) {
    const bool exact_rows =
        out.expected_match_count > 0
        && out.match_count == out.expected_match_count
        && out.exact_primary_count_accepted == 1;
    const bool all_expected_correct =
        out.expected_match_count > 0
        && out.correct_match_count == out.expected_match_count
        && out.correct_strand_count == out.expected_match_count;
    const bool cigar_ok =
        !in.cigar_projection
        || (out.correct_cigar_count == out.expected_match_count
            && out.cigar_replay_count == out.expected_match_count);
    if (out.found
        && exact_rows
        && all_expected_correct
        && cigar_ok
        && out.abs_offset_bp < static_cast<double>(kPositionToleranceBp)) {
        return;
    }
    bioseq_log<ERR>("[bench_e2e] {} failed to map the query at {} bp (found={}, "
         "target_start={}, truth={}, offset={} bp >= {} threshold) — the timed result is "
         "invalid; aborting (match_count={}, expected={}, correct={}, "
         "correct_strand={}, correct_cigar={}, cigar_replay={}, cigar_projection={})",
         algo, in.target_bp, out.found, out.target_start,
         in.truth_target_start, out.abs_offset_bp, kPositionToleranceBp,
         out.match_count, out.expected_match_count, out.correct_match_count,
         out.correct_strand_count, out.correct_cigar_count,
         out.cigar_replay_count, in.cigar_projection);
    // fmtlog is async; the ERR above can be lost when std::abort() runs before
    // the background poll flushes it. Emit a synchronous copy so the failing
    // cell is always visible in the captured stderr.
    std::fprintf(stderr,
        "[bench_e2e] MAP-FAIL algo=%.*s target_bp=%llu found=%d target_start=%lld "
        "truth=%lld offset_bp=%.1f tol=%lld match_count=%lld expected=%lld "
        "correct=%lld correct_strand=%lld correct_cigar=%lld cigar_replay=%lld "
        "cigar_projection=%d — aborting\n",
        static_cast<int>(algo.size()), algo.data(),
        static_cast<unsigned long long>(in.target_bp), static_cast<int>(out.found),
        static_cast<long long>(out.target_start),
        static_cast<long long>(in.truth_target_start),
        out.abs_offset_bp, static_cast<long long>(kPositionToleranceBp),
        static_cast<long long>(out.match_count),
        static_cast<long long>(out.expected_match_count),
        static_cast<long long>(out.correct_match_count),
        static_cast<long long>(out.correct_strand_count),
        static_cast<long long>(out.correct_cigar_count),
        static_cast<long long>(out.cigar_replay_count),
        static_cast<int>(in.cigar_projection));
    std::fflush(stderr);
    std::abort();
}

struct PipelineProofExpectation {
    std::int64_t target_rows = 0;
    std::int64_t attempted_alignment_jobs = 0;
    std::int64_t target_rows_per_dispatch = 0;
    std::int64_t effective_cpu_workers = 0;
};

[[nodiscard]] std::int64_t checked_pipeline_job_product(
        const std::size_t target_rows,
        const std::size_t query_count) {
    constexpr auto limit = static_cast<std::size_t>(
        (std::numeric_limits<std::int64_t>::max)());
    if (target_rows > limit
        || (target_rows != 0 && query_count > limit / target_rows)) {
        std::fprintf(
            stderr,
            "[bench_e2e] PIPELINE-PROOF-EXPECTATION-OVERFLOW "
            "targets=%zu queries=%zu\n",
            target_rows,
            query_count);
        std::fflush(stderr);
        std::abort();
    }
    return static_cast<std::int64_t>(target_rows * query_count);
}

[[nodiscard]] PipelineProofExpectation pipeline_proof_expectation(
        const E2eInput& in,
        const bool just_index,
        const int effective_cpu_workers) {
    // Both cold modes use exactly one physical target row and N independently
    // materialized query rows through BioseqBatchJoin. Physical target
    // duplication is not a supported benchmark compatibility mode.
    (void)just_index;
    constexpr std::size_t target_rows_per_dispatch = 1;
    const std::size_t queries_per_target_dispatch = in.queries.size();
    const bool headline_comparison_case =
        in.size_label.starts_with("2gbp_")
        || in.size_label.starts_with("4gbp_");
    if (headline_comparison_case && queries_per_target_dispatch != 16) {
        std::fprintf(
            stderr,
            "[bench_e2e] PIPELINE-PROOF-SHAPE-FAIL headline case '%s' "
            "requires target_rows=1/query_rows=16; actual query_rows=%zu\n",
            in.size_label.c_str(),
            queries_per_target_dispatch);
        std::fflush(stderr);
        std::abort();
    }
    const std::int64_t expected_jobs = checked_pipeline_job_product(
        target_rows_per_dispatch,
        queries_per_target_dispatch);
    return PipelineProofExpectation{
        .target_rows = static_cast<std::int64_t>(target_rows_per_dispatch),
        .attempted_alignment_jobs = expected_jobs,
        .target_rows_per_dispatch = static_cast<std::int64_t>(
            target_rows_per_dispatch),
        .effective_cpu_workers = effective_cpu_workers,
    };
}

[[nodiscard]] bool validate_alignment_stage_matrix(
        const E2eOutput& out,
        const bioseq::pipeline_stats::ExecutedBackend expected_backend,
        const bioseq::pipeline_stats::TargetIndexSource expected_source) {
    using bioseq::pipeline_stats::ExecutionEngine;
    using bioseq::pipeline_stats::PipelineStage;
    using bioseq::pipeline_stats::engine_index;
    using bioseq::pipeline_stats::expected_execution_engine;
    using bioseq::pipeline_stats::stage_requires_front_end_eligibility;

    if (out.alignment_snapshot_rows.size()
            != E2eOutput::kAlignmentSnapshotRowCount
        || out.attempted_alignment_jobs <= 0) {
        return false;
    }
    const auto attempted = out.attempted_alignment_jobs;
    for (std::size_t row_index = 0;
         row_index < out.alignment_snapshot_rows.size(); ++row_index) {
        const auto& row = out.alignment_snapshot_rows[row_index];
        if (row.size() != E2eOutput::kAlignmentSnapshotColumnCount) {
            return false;
        }
        const auto eligible = parse_snapshot_i64(row[24]);
        const auto host = parse_snapshot_i64(row[25]);
        const auto scalar = parse_snapshot_i64(row[26]);
        const auto simd = parse_snapshot_i64(row[27]);
        const auto vulkan = parse_snapshot_i64(row[28]);
        const auto fallback = parse_snapshot_i64(row[29]);
        const auto gpu_device_ns = parse_snapshot_i64(row[31]);
        if (!eligible.has_value() || !host.has_value()
            || !scalar.has_value() || !simd.has_value()
            || !vulkan.has_value() || !fallback.has_value()
            || !gpu_device_ns.has_value()
            || *eligible > attempted || *fallback != 0) {
            return false;
        }
        const std::array<std::int64_t, 4> executed{
            *host, *scalar, *simd, *vulkan,
        };
        const auto expected_engine = expected_execution_engine(
            expected_backend,
            expected_source,
            static_cast<PipelineStage>(row_index));
        if (expected_engine == ExecutionEngine::count) {
            if (*eligible != 0
                || std::any_of(executed.begin(), executed.end(),
                    [](const std::int64_t count) { return count != 0; })
                || *gpu_device_ns != 0) {
                return false;
            }
            continue;
        }
        if (stage_requires_front_end_eligibility(
                expected_source,
                static_cast<PipelineStage>(row_index))
            && *eligible != attempted) {
            return false;
        }
        for (std::size_t engine = 0; engine < executed.size(); ++engine) {
            const std::int64_t expected =
                engine == engine_index(expected_engine) ? *eligible : 0;
            if (executed[engine] != expected) {
                return false;
            }
        }
        if (expected_engine != ExecutionEngine::vulkan_gpu
            && *gpu_device_ns != 0) {
            return false;
        }
    }
    return true;
}

inline void require_pipeline_execution_proof(
        std::string_view algo,
        const E2eInput& in,
        E2eOutput& out,
        const PipelineProofExpectation& expectation,
        std::string_view minimizer_index) {
    const bool expect_gpu =
        algo.find("_gpu_") != std::string_view::npos
        || algo.find("_vulkan_") != std::string_view::npos;
    const bool expect_simd =
        algo.find("_simd_") != std::string_view::npos;
    const bool expect_scalar =
        algo.find("_scalar_") != std::string_view::npos;
    if (!expect_gpu && !expect_simd && !expect_scalar) {
        return;
    }
    const bool expect_persisted =
        algo.ends_with("_just_index");
    const bool expect_compact_h32_direct =
        expect_persisted && expect_simd
        && minimizer_index == kMinimizersPerFragmentIndex;
    const std::string_view expected_backend = expect_gpu
        ? "vulkan_gpu"
        : expect_simd
            ? "simd_cpu"
            : "scalar_cpu";
    const std::string_view expected_source =
        expect_persisted ? "persisted" : "recomputed";
    const auto expected_backend_enum = expect_gpu
        ? bioseq::pipeline_stats::ExecutedBackend::vulkan_gpu
        : expect_simd
            ? bioseq::pipeline_stats::ExecutedBackend::simd_cpu
            : bioseq::pipeline_stats::ExecutedBackend::scalar_cpu;
    const auto expected_source_enum = expect_persisted
        ? bioseq::pipeline_stats::TargetIndexSource::persisted
        : bioseq::pipeline_stats::TargetIndexSource::recomputed;
    out.stage_engine_matrix_valid = validate_alignment_stage_matrix(
        out, expected_backend_enum, expected_source_enum) ? 1 : 0;
    const bool row_equation =
        out.target_rows >= 0
        && out.target_rows
            == out.persisted_index_rows
                + out.recomputed_target_index_rows
        && out.missing_persisted_index_rows
            <= out.recomputed_target_index_rows;
    const bool source_valid = expect_persisted
        ? out.persisted_index_rows == out.target_rows
            && out.missing_persisted_index_rows == 0
            && out.recomputed_target_index_rows == 0
        : out.persisted_index_rows == 0
            && out.missing_persisted_index_rows == 0
            && out.recomputed_target_index_rows == out.target_rows;
    const bool persisted_identity_valid = expect_persisted
        ? out.persisted_generation_id > 0
            && fixed_lower_hex(out.persisted_source_identity, 16)
            && out.persisted_source_identity != "0000000000000000"
        : out.persisted_generation_id == 0
            && out.persisted_source_identity == "0000000000000000";
    const bool backend_specific = expect_gpu
        ? out.gpu_seed_dispatches > 0
            && out.gpu_chain_dispatches > 0
            && out.gpu_chain_backtrack_dispatches > 0
            && (!in.cigar_projection || out.gpu_extend_dispatches > 0)
            && out.host_query_hash_readbacks == 0
            && out.host_query_symbol_packing_calls == 0
            && out.host_target_symbol_packing_calls == 0
            && out.host_alignment_job_classification_calls == 0
            && out.host_cigar_construction_calls == 0
        : out.gpu_seed_dispatches == 0
            && out.gpu_chain_dispatches == 0
            && out.gpu_chain_backtrack_dispatches == 0
            && out.gpu_extend_dispatches == 0;
    const bool exact_cardinality =
        out.target_rows == expectation.target_rows
        && out.attempted_alignment_jobs
            == expectation.attempted_alignment_jobs;
    // A genuine BatchJoin batch is one target crossed with multiple logical
    // query jobs; physical target duplication is not an acceptance signal.
    const bool gpu_signoff_shape =
        !expect_gpu || expectation.attempted_alignment_jobs >= 2;
    const auto counter = [&out](const std::size_t column) noexcept {
        return out.alignment_snapshot_numeric_columns[column];
    };
    const auto sum_equal = [](const std::int64_t total,
                              const std::int64_t lhs,
                              const std::int64_t rhs) noexcept {
        return static_cast<__int128>(total)
            == static_cast<__int128>(lhs) + static_cast<__int128>(rhs);
    };
    const auto scaled_equal = [](const std::int64_t total,
                                 const std::int64_t count,
                                 const std::int64_t width) noexcept {
        return static_cast<__int128>(total)
            == static_cast<__int128>(count) * static_cast<__int128>(width);
    };
    const bool expect_format6 =
        expect_persisted && e2e_expects_format6(in, minimizer_index);
    const bool expect_format5 =
        expect_persisted && !expect_format6;
    out.persisted_format_version = expect_format6
        ? 6 : expect_format5 ? 5 : 0;
    const bool persisted_manifest_size_valid =
        scaled_equal(
            counter(56), counter(55),
            bioseq::indexing::MinimizerGenerationManifest::
                format5_encoded_bytes);
    out.persisted_format5_manifest_contract_valid = expect_format5
        && out.pre_restart_persisted_gpu_abi_version == 3
        && counter(55) == out.persisted_generation_read_sessions
        && persisted_manifest_size_valid
            ? 1
            : 0;
    const bool cold_payload_equations =
        out.cold_index_payload_rows_requested
            == out.cold_index_payload_rows_delivered
        && out.cold_index_payload_rows_delivered >= 0
        && out.cold_index_payload_rows_scanned
            >= out.cold_index_payload_rows_delivered
        && sum_equal(
            out.cold_index_payload_rows_scanned,
            out.cold_index_payload_rows_delivered,
            out.cold_index_payload_rows_skipped)
        && out.cold_index_payload_bytes_requested
            == out.cold_index_payload_bytes_delivered
        && out.cold_index_payload_bytes_scanned
            >= out.cold_index_payload_bytes_delivered;
    const bool mapped_format5_cold_route =
        expect_format5
        && out.cold_index_route_empty == 0
        && out.cold_index_route_selective == 0
        && out.cold_index_route_full == 0;
    const bool one_cold_route =
        mapped_format5_cold_route
        || (!expect_format5
        && out.cold_index_route_empty >= 0
        && out.cold_index_route_selective >= 0
        && out.cold_index_route_full >= 0
        && static_cast<__int128>(out.cold_index_route_empty)
                + static_cast<__int128>(out.cold_index_route_selective)
                + static_cast<__int128>(out.cold_index_route_full)
            == static_cast<__int128>(1));
    const bool selective_read_gate =
        out.cold_index_route_selective == 1
        && out.cold_index_route_empty == 0
        && out.cold_index_route_full == 0
        && out.cold_index_route_candidate_rows > 0
        && out.cold_index_route_candidate_rows
            < out.cold_index_route_full_rows
        && out.cold_index_route_candidate_bytes > 0
        && out.cold_index_route_candidate_bytes
            < out.cold_index_route_full_bytes
        && out.cold_index_route_selective_cost
            < out.cold_index_route_full_cost
        && out.cold_index_payload_rows_requested > 0
        && out.cold_index_payload_rows_requested
            <= out.cold_index_route_candidate_rows
        && out.cold_index_payload_bytes_requested > 0
        && out.cold_index_payload_bytes_requested
            <= out.cold_index_route_candidate_bytes
        && counter(78) > 0
        && counter(78) == counter(79)
        && counter(80) > 0;
    const bool full_read_gate =
        out.cold_index_route_full == 1
        && out.cold_index_route_empty == 0
        && out.cold_index_route_selective == 0
        && out.cold_index_route_full_rows > 0
        && out.cold_index_route_full_bytes > 0
        && out.cold_index_payload_rows_requested
            == out.cold_index_route_full_rows
        && out.cold_index_payload_bytes_requested
            == out.cold_index_route_full_bytes
        && out.extent_crc_raw_bytes_hashed == 0
        && out.extent_crc_combined_bytes
            == out.cold_index_payload_bytes_delivered
        && out.extent_crc_combine_rows
            == out.cold_index_payload_rows_delivered
        && counter(80) == 1;
    const bool format5_mapped_image_read_gate =
        out.cold_index_schema_valid == 1
        && out.cold_index_route_empty == 0
        && out.cold_index_route_selective == 0
        && out.cold_index_route_full == 0
        && out.cold_index_route_candidate_rows == 0
        && out.cold_index_route_candidate_bytes == 0
        && out.cold_index_route_selective_cost == 0
        && (expect_compact_h32_direct
            ? out.cold_index_route_full_rows == 0
                && out.cold_index_route_full_bytes == 0
                && out.cold_index_route_full_cost == 0
            : out.cold_index_route_full_rows > 0
                && out.cold_index_route_full_bytes > 0
                && out.cold_index_route_full_cost > 0)
        && out.cold_index_payload_rows_requested == 0
        && out.cold_index_payload_rows_delivered == 0
        && out.cold_index_payload_rows_scanned == 0
        && out.cold_index_payload_rows_skipped == 0
        && out.cold_index_payload_bytes_requested == 0
        && out.cold_index_payload_bytes_delivered == 0
        && out.cold_index_payload_bytes_scanned == 0;
    const bool format5_metadata_read_shape =
        expect_compact_h32_direct
        ? counter(57) == 0 && counter(58) == 0
        : counter(57) > 0 && counter(58) > 0;
    const bool format5_persisted_read_gate = expect_format5
        ? out.cold_index_schema_valid == 1
            && format5_mapped_image_read_gate
            && cold_payload_equations
            && out.persisted_generation_read_sessions == 1
            && out.persisted_generation_read_sessions == out.target_rows
            && out.persisted_relation_opens == out.target_rows
            && counter(51) == out.persisted_generation_read_sessions
            && counter(52) == out.persisted_generation_read_sessions
            && scaled_equal(counter(53), counter(52), 64)
            && counter(54) == out.persisted_generation_read_sessions
            && counter(55) == out.persisted_generation_read_sessions
            && out.persisted_manifest_rows_read == counter(55)
            && out.persisted_manifest_bytes_read == counter(56)
            && out.persisted_format5_manifest_contract_valid == 1
            && persisted_manifest_size_valid
            && format5_metadata_read_shape
            && counter(59) == 0
            && static_cast<__int128>(counter(60))
                == static_cast<__int128>(counter(52))
                    + static_cast<__int128>(counter(55))
                    + static_cast<__int128>(counter(57))
            && static_cast<__int128>(counter(61))
                == static_cast<__int128>(counter(53))
                    + static_cast<__int128>(counter(56))
                    + static_cast<__int128>(counter(58))
            && counter(62) == 0
            && counter(63) == 0
            && counter(64) == counter(60)
            && counter(67) == counter(61)
            && counter(67) > 0
            && counter(68) == 0
            && counter(69) == 0
            && counter(70) == 0
            && counter(71) == 0
            && counter(72) == 0
            && counter(73) == 0
            && counter(74) == 0
            && counter(75) == 0
            && counter(76) == 0
            && counter(77) == 0
            && counter(78) == 0
            && counter(79) == 0
            && counter(80) == 0
            && counter(81) == 0
            && static_cast<__int128>(out.persisted_index_scans)
                == static_cast<__int128>(counter(51))
                    + static_cast<__int128>(counter(54))
            && sum_equal(counter(64), counter(60), counter(62))
            && counter(65) == 0
            && counter(66) == 0
            && counter(82) == 0
            && counter(83) == 0
            && counter(84) == 0
            && counter(85) == 0
        : true;
    const bool no_persisted_read_gate = expect_persisted
        || (out.cold_index_schema_valid == 1
            && out.cold_index_route_empty == 0
            && out.cold_index_route_selective == 0
            && out.cold_index_route_full == 0
            && out.cold_index_payload_rows_requested == 0
            && out.cold_index_payload_rows_delivered == 0
            && out.cold_index_payload_rows_scanned == 0
            && out.cold_index_payload_rows_skipped == 0
            && out.cold_index_payload_bytes_requested == 0
            && out.cold_index_payload_bytes_delivered == 0
            && out.cold_index_payload_bytes_scanned == 0
            && std::all_of(
                out.alignment_snapshot_numeric_columns.begin() + 48,
                out.alignment_snapshot_numeric_columns.begin() + 84,
                [](const std::int64_t value) noexcept {
                    return value == 0;
                }));
    // Keep these diagnostic groups mathematically identical to the persisted
    // branch above.  They do not participate in acceptance; they only make a
    // failed cold-reader proof identify its exact class of disagreement
    // without another opaque E2E run.
    const bool persisted_read_route_proof =
        expect_format5
            ? format5_mapped_image_read_gate
                && cold_payload_equations
            : out.cold_index_schema_valid == 1
                && one_cold_route
                && (selective_read_gate || full_read_gate)
                && cold_payload_equations;
    const bool persisted_read_session_proof =
        out.persisted_generation_read_sessions == 1
        && out.persisted_generation_read_sessions == out.target_rows
        && out.persisted_relation_opens == out.target_rows;
    const bool persisted_read_head_proof =
        counter(51) == out.persisted_generation_read_sessions
        && counter(52) == out.persisted_generation_read_sessions
        && scaled_equal(counter(53), counter(52), 64);
    const bool persisted_read_manifest_proof =
        counter(54) == out.persisted_generation_read_sessions
        && counter(55) == out.persisted_generation_read_sessions
        && out.persisted_manifest_rows_read == counter(55)
        && out.persisted_manifest_bytes_read == counter(56)
        && out.persisted_format5_manifest_contract_valid == 1
        && persisted_manifest_size_valid;
    const bool persisted_read_directory_proof =
        format5_metadata_read_shape
        && counter(59) == 0
        && static_cast<__int128>(counter(60))
            == static_cast<__int128>(counter(52))
                + static_cast<__int128>(counter(55))
                + static_cast<__int128>(counter(57))
        && static_cast<__int128>(counter(61))
            == static_cast<__int128>(counter(53))
                + static_cast<__int128>(counter(56))
                + static_cast<__int128>(counter(58));
    const bool persisted_read_payload_proof =
        expect_format5
            ? counter(62) == 0
                && counter(63) == 0
                && counter(67) == counter(61)
                && std::all_of(
                    out.alignment_snapshot_numeric_columns.begin() + 68,
                    out.alignment_snapshot_numeric_columns.begin() + 82,
                    [](const std::int64_t value) noexcept {
                        return value == 0;
                    })
            : counter(62) == out.cold_index_payload_rows_delivered
                && counter(63) == out.cold_index_payload_bytes_delivered
                && counter(63) > 0
                && counter(67) > 0
                && counter(68) > 0
                && counter(68) == counter(69)
                && counter(70) > 0
                && counter(70) == counter(74)
                && counter(71) > 0
                && counter(71) == counter(75)
                && counter(76) > 0
                && counter(77) >= 1
                && (out.cold_index_route_full != 1
                    || (out.extent_crc_raw_bytes_hashed == 0
                        && out.extent_crc_combined_bytes
                            == out.cold_index_payload_bytes_delivered
                        && out.extent_crc_combine_rows
                            == out.cold_index_payload_rows_delivered));
    const bool persisted_read_scan_equations =
        static_cast<__int128>(out.persisted_index_scans)
            == static_cast<__int128>(counter(51))
                + static_cast<__int128>(counter(54))
                + static_cast<__int128>(counter(80))
        && sum_equal(counter(64), counter(60), counter(62))
        && counter(76) == out.cold_index_payload_bytes_delivered;
    const bool persisted_read_zero_work_proof =
        counter(65) == 0
        && counter(66) == 0
        && counter(73) == 0
        && counter(82) == 0
        && counter(84) == 0
        && counter(85) == 0;
    const bool no_format5_reader_memory_evidence =
        out.format5_reader_capacity_accounting_passes == 0
        && out.format5_reader_global_position_storage_preparations == 0
        && out.format5_reader_global_position_storage_allocations == 0
        && out.format5_reader_position_append_growth_reservations == 0
        && out.format5_reader_retained_position_count == 0
        && out.format5_reader_retained_position_capacity == 0
        && out.format5_reader_directory_capacity_bytes == 0
        && out.format5_reader_query_hash_capacity_bytes == 0
        && out.format5_reader_query_record_capacity_bytes == 0
        && out.format5_reader_ordinal_route_capacity_bytes == 0
        && out.format5_reader_selected_extent_capacity_bytes == 0
        && out.format5_reader_current_selected_control_capacity_bytes == 0
        && out.format5_reader_global_position_capacity_bytes == 0
        && out.format5_reader_control_payload_capacity_bytes == 0
        && out.format5_reader_owned_capacity_bytes == 0
        && out.format5_reader_admitted_bytes == 0
        && out.format5_reader_exact_position_image_checks == 0
        && out.format5_reader_exact_position_image_failures == 0
        && out.format5_reader_retained_layout_checks == 0
        && out.format5_reader_retained_layout_failures == 0
        && out.format5_reader_memory_evidence_saturated == 0;
    const bool no_format6_reader_evidence = [&out] {
        const auto& evidence = out.format6_reader_evidence;
        if (required_json_counter(evidence, "format_version") != 0
            || required_json_counter(evidence, "sessions") != 0
            || required_json_counter(evidence, "generation_id") != 0
            || required_json_text(evidence, "source_identity")
                != "00000000000000000000000000000000"
            || required_json_text(evidence, "source_content_crc64")
                != "0000000000000000"
            || required_json_text(evidence, "compatibility_digest")
                != std::string(64, '0')
            || evidence["saturated"].get<bool>()
            || evidence["operational_identity_conflict"].get<bool>()
            || evidence["stats_saturated"].get<bool>()) {
            return false;
        }
        constexpr auto zero_keys = std::to_array<std::string_view>({
            "directory_rows", "directory_bytes", "control_rows",
            "control_bytes", "posting_rows", "posting_bytes",
            "directory_row_crc_receipts",
            "directory_stream_crc_receipts", "control_row_crc_receipts",
            "control_block_crc_receipts", "control_semantic_receipts",
            "posting_row_crc_receipts", "posting_ranges_delivered",
            "posting_range_crc_receipts", "posting_semantic_receipts",
            "authorized_posting_bytes", "query_count",
            "selected_block_count", "accepted_hash_count",
            "retained_position_count", "directory_phase_peak_bytes",
            "control_phase_peak_bytes", "posting_phase_peak_bytes",
            "peak_admitted_bytes", "retained_owner_bytes",
            "admitted_bytes", "legacy_assembly_calls",
            "legacy_globalize_calls", "legacy_rebase_calls",
            "generic_source_fallback_calls", "backend_fallbacks",
            "failures",
        });
        return std::ranges::all_of(zero_keys, [&evidence](const auto key) {
            return required_json_counter(evidence, key) == 0;
        });
    }();
    const bool format6_persisted_read_gate = !expect_format6 || [&] {
        const auto& evidence = out.format6_reader_evidence;
        const auto& audit = out.post_restart_format6_ready_audit;
        const auto& source = required_json_object(audit, "source");
        const auto& head = required_json_object(audit, "head");
        const auto& manifest = required_json_object(audit, "manifest");
        const auto& compatibility =
            required_json_object(manifest, "compatibility");
        const auto& streams = required_json_array(audit, "streams");
        const auto f6 = [&evidence](const std::string_view key) {
            return required_json_counter(evidence, key);
        };
        const auto audit_counter = [](const nlohmann::json& object,
                                      const std::string_view key) {
            return required_json_counter(object, key);
        };
        const std::int64_t directory_rows =
            audit_counter(streams[0], "row_count");
        const std::int64_t directory_bytes =
            audit_counter(streams[0], "logical_bytes");
        const std::int64_t posting_width =
            audit_counter(compatibility, "stored_posting_width_bytes");
        const std::int64_t phase_peak = std::max({
            f6("directory_phase_peak_bytes"),
            f6("control_phase_peak_bytes"),
            f6("posting_phase_peak_bytes"),
        });
        const bool physical_metadata =
            out.persisted_generation_read_sessions == 1
            && out.persisted_generation_read_sessions == out.target_rows
            && out.persisted_relation_opens == out.target_rows
            && counter(51) == 1
            && counter(52) == 1
            && counter(53)
                == bioseq::indexing::kMinimizerFormat6HeadBytes
            && counter(54) == 1
            && counter(55) == 1
            && counter(56)
                == bioseq::indexing::kMinimizerFormat6ManifestBytes
            && counter(142) == 1
            && counter(143) == 1
            && counter(144) == 1
            && counter(145) == 0
            && counter(146) == 0
            && counter(147) == 0
            && counter(148) == 0
            && counter(82) == 0;
        const bool runtime_identity =
            f6("format_version") == 6
            && f6("sessions") == 1
            && f6("generation_id")
                == audit_counter(head, "generation_id")
            && f6("generation_id") == out.persisted_generation_id
            && required_json_text(evidence, "source_identity")
                == required_json_text(source, "source_identity")
            && required_json_text(evidence, "source_content_crc64")
                == required_json_text(source, "source_content_crc64")
            && required_json_text(evidence, "compatibility_digest")
                == required_json_text(head, "compatibility_digest");
        const bool directory_receipts =
            f6("directory_rows") == directory_rows
            && f6("directory_bytes") == directory_bytes
            && f6("directory_row_crc_receipts") == directory_rows
            && f6("directory_stream_crc_receipts") == 1;
        const bool control_receipts =
            f6("control_rows") > 0
            && f6("control_bytes") > 0
            && f6("control_bytes") >= f6("control_rows")
            && static_cast<__int128>(f6("control_bytes"))
                <= static_cast<__int128>(f6("control_rows"))
                    * static_cast<__int128>(
                        bioseq::indexing::
                            kMinimizerFormat6RowPayloadBytes)
            && f6("control_row_crc_receipts") == f6("control_rows")
            && f6("selected_block_count") > 0
            && f6("control_block_crc_receipts")
                == f6("selected_block_count")
            && f6("control_semantic_receipts")
                == f6("selected_block_count");
        const bool posting_receipts =
            f6("posting_rows") > 0
            && f6("posting_bytes") > 0
            && f6("posting_bytes") >= f6("posting_rows")
            && static_cast<__int128>(f6("posting_bytes"))
                <= static_cast<__int128>(f6("posting_rows"))
                    * static_cast<__int128>(
                        bioseq::indexing::
                            kMinimizerFormat6RowPayloadBytes)
            && f6("posting_row_crc_receipts") == f6("posting_rows")
            && f6("posting_ranges_delivered") > 0
            && f6("posting_semantic_receipts")
                == f6("posting_ranges_delivered")
            && f6("posting_range_crc_receipts")
                <= f6("posting_ranges_delivered")
            && f6("retained_position_count") > 0
            && static_cast<__int128>(f6("authorized_posting_bytes"))
                == static_cast<__int128>(
                    f6("retained_position_count"))
                    * static_cast<__int128>(posting_width);
        const bool selective_work =
            f6("query_count") > 0
            && f6("accepted_hash_count") > 0
            && f6("accepted_hash_count") <= f6("query_count")
            && f6("selected_block_count")
                <= audit_counter(manifest, "directory_entry_count");
        const bool exact_memory =
            f6("directory_phase_peak_bytes") > 0
            && f6("control_phase_peak_bytes") > 0
            && f6("posting_phase_peak_bytes") > 0
            && f6("peak_admitted_bytes") == phase_peak
            && f6("retained_owner_bytes") > 0
            && f6("retained_owner_bytes") == f6("admitted_bytes");
        const bool forbidden_routes =
            f6("legacy_assembly_calls") == 0
            && f6("legacy_globalize_calls") == 0
            && f6("legacy_rebase_calls") == 0
            && f6("recompute_calls") == 0
            && f6("generic_source_fallback_calls") == 0
            && f6("backend_fallbacks") == 0
            && f6("failures") == 0
            && !evidence["saturated"].get<bool>()
            && !evidence["operational_identity_conflict"].get<bool>()
            && !evidence["stats_saturated"].get<bool>();
        return physical_metadata && runtime_identity
            && directory_receipts && control_receipts
            && posting_receipts && selective_work && exact_memory
            && forbidden_routes;
    }();
    out.persisted_format6_contract_valid =
        expect_format6 && format6_persisted_read_gate ? 1 : 0;
    const bool persisted_read_gate = expect_format6
        ? format6_persisted_read_gate
        : expect_format5
            ? format5_persisted_read_gate && no_format6_reader_evidence
            : no_persisted_read_gate && no_format6_reader_evidence;
    const __int128 format5_reader_sessions =
        static_cast<__int128>(out.persisted_generation_read_sessions);
    const __int128 format5_reader_passes = static_cast<__int128>(
        out.format5_reader_capacity_accounting_passes);
    /* MPF performs one fixed accounting pass. The compact-direct-only route
     * owns the nested direct mapping authority but no Format-5 directory,
     * payload, entry plan, or block plan. */
    const bool format5_reader_accounting_shape =
        format5_reader_sessions > 0
        && format5_reader_passes == format5_reader_sessions;
    const __int128 format5_reader_exact_position_bytes =
        static_cast<__int128>(out.format5_reader_retained_position_count)
            * static_cast<__int128>(sizeof(std::uint64_t));
    const __int128 format5_reader_layout_bytes =
        static_cast<__int128>(out.format5_reader_directory_capacity_bytes)
        + static_cast<__int128>(out.format5_reader_query_hash_capacity_bytes)
        + static_cast<__int128>(out.format5_reader_query_record_capacity_bytes)
        + static_cast<__int128>(out.format5_reader_ordinal_route_capacity_bytes)
        + static_cast<__int128>(out.format5_reader_selected_extent_capacity_bytes)
        + static_cast<__int128>(
            out.format5_reader_current_selected_control_capacity_bytes)
        + static_cast<__int128>(
            out.format5_reader_global_position_capacity_bytes)
        + static_cast<__int128>(
            out.format5_reader_control_payload_capacity_bytes);
    const auto fragment_native = [&out](const std::string_view key) {
        return required_json_counter(
            out.format5_reader_memory_evidence, key);
    };
    const bool no_fragment_native_query_evidence =
        std::ranges::all_of(
            kFragmentNativeEvidenceCounterKeys,
            [&fragment_native](const std::string_view key) {
                return fragment_native(key) == 0;
            })
        && !out.format5_reader_memory_evidence[
            "fragment_native_evidence_saturated"].get<bool>();
    const bool canonical_p3_packed_single_wave =
        in.size_label != "2gbp_dna4_mpf_fragment_local_focus"
        || minimizer_index != kMinimizersPerFragmentIndex
        || (fragment_native("fragment_native_waves") == 1
            && fragment_native("fragment_native_queries") == 16
            && fragment_native(
                    "fragment_native_packed_occurrence_waves") == 1
            && fragment_native(
                    "fragment_native_wide_occurrence_waves") == 0);
    const bool compact_direct_one_decode_gate = expect_compact_h32_direct
        ? fragment_native(
                "fragment_native_compact_direct_selection_shards") == 32
            && fragment_native(
                    "fragment_native_compact_direct_layout_shards_completed")
                == fragment_native(
                    "fragment_native_compact_direct_selection_shards")
            && fragment_native(
                    "fragment_native_compact_direct_emission_shards_completed")
                == fragment_native(
                    "fragment_native_compact_direct_selection_shards")
            && static_cast<__int128>(fragment_native(
                    "fragment_native_compact_direct_shard_query_layouts"))
                == static_cast<__int128>(fragment_native(
                    "fragment_native_compact_direct_selection_shards"))
                    * static_cast<__int128>(fragment_native(
                        "fragment_native_queries"))
            && fragment_native(
                    "fragment_native_compact_direct_posting_visit_passes")
                == 1
            && fragment_native(
                    "fragment_native_compact_direct_selected_postings") > 0
            && fragment_native(
                    "fragment_native_compact_direct_layout_postings_visited")
                == 0
            && fragment_native(
                    "fragment_native_compact_direct_emission_postings_visited")
                == fragment_native(
                    "fragment_native_compact_direct_selected_postings")
            && fragment_native(
                    "fragment_native_compact_direct_target_control_scan_bytes")
                == 0
            && fragment_native(
                    "fragment_native_compact_direct_target_control_scan_passes")
                == 0
            && fragment_native(
                    "fragment_native_compact_direct_fallbacks") == 0
            && fragment_native(
                    "fragment_native_compact_direct_traversal_rank_checkpoints_examined")
                == 0
            && fragment_native(
                    "fragment_native_compact_direct_traversal_membership_words_examined")
                == 0
            && fragment_native(
                    "fragment_native_compact_direct_rejected_posting_reads")
                == 0
        : std::ranges::all_of(
            kFormat5ReaderMemoryEvidenceV6AdditionKeys,
            [&fragment_native](const std::string_view key) {
                return fragment_native(key) == 0;
            });
    const bool fragment_native_query_gate = expect_format5 && !expect_gpu
        ? canonical_p3_packed_single_wave
            && compact_direct_one_decode_gate
            && fragment_native("fragment_native_waves") > 0
            && fragment_native("fragment_native_queries")
                == expectation.attempted_alignment_jobs
            && fragment_native("fragment_native_queries")
                == fragment_native(
                    "fragment_native_global_sort_calls_skipped")
            && fragment_native(
                    "fragment_native_global_sort_calls_invoked") == 0
            && fragment_native("fragment_native_query_occurrences") > 0
            && fragment_native("fragment_native_unique_query_plans") > 0
            && fragment_native("fragment_native_query_occurrences")
                >= fragment_native("fragment_native_unique_query_plans")
            && static_cast<__int128>(fragment_native(
                    "fragment_native_packed_occurrence_waves"))
                    + static_cast<__int128>(fragment_native(
                        "fragment_native_wide_occurrence_waves"))
                == static_cast<__int128>(fragment_native(
                    "fragment_native_waves"))
            && fragment_native(
                    "fragment_native_packed_occurrence_waves")
                == fragment_native("fragment_native_waves")
            && fragment_native(
                    "fragment_native_wide_occurrence_waves") == 0
            && (expect_compact_h32_direct
                ? fragment_native(
                        "fragment_native_control_count_bytes") == 0
                : fragment_native(
                        "fragment_native_control_count_bytes") > 0)
            && (expect_compact_h32_direct
                ? fragment_native(
                        "fragment_native_pass2_position_bytes") == 0
                    && fragment_native(
                        "fragment_native_pass3_position_bytes") > 0
                : fragment_native(
                        "fragment_native_pass2_position_bytes") > 0
                    && fragment_native(
                        "fragment_native_pass2_position_bytes")
                        == fragment_native(
                            "fragment_native_pass3_position_bytes"))
            && fragment_native("fragment_native_anchors_constructed") > 0
            && static_cast<__int128>(
                    fragment_native("fragment_native_exact_final_bytes"))
                == static_cast<__int128>(
                    fragment_native("fragment_native_anchors_constructed"))
                    * static_cast<__int128>(sizeof(bioseq::chaining::Anchor))
            && fragment_native("fragment_native_peak_exact_final_bytes") > 0
            && fragment_native("fragment_native_peak_exact_final_bytes")
                <= fragment_native("fragment_native_exact_final_bytes")
            && fragment_native("fragment_native_local_sort_bytes") > 0
            && fragment_native("fragment_native_candidate_anchor_bytes") == 0
            && fragment_native("fragment_native_plan_ordinal_bytes") == 0
            && fragment_native("fragment_native_compaction_bytes") == 0
            && fragment_native("fragment_native_final_copy_bytes") == 0
            && fragment_native("fragment_native_occurrence_fill_ns") > 0
            && fragment_native("fragment_native_occurrence_radix_ns") > 0
            && fragment_native("fragment_native_unique_plan_build_ns") > 0
            && (expect_compact_h32_direct
                ? fragment_native(
                        "fragment_native_query_plan_radix_fill_ns") == 0
                : fragment_native(
                        "fragment_native_query_plan_radix_fill_ns") > 0)
            && (expect_compact_h32_direct
                ? fragment_native(
                        "fragment_native_control_count_pass_ns") == 0
                : fragment_native(
                        "fragment_native_control_count_pass_ns") > 0)
            && fragment_native("fragment_native_orientation_layout_ns") > 0
            && fragment_native("fragment_native_direct_emission_ns") > 0
            && fragment_native("fragment_native_local_sort_ns") > 0
            && fragment_native("fragment_native_tie_normalization_ns") > 0
            && fragment_native(
                    "fragment_native_peak_occurrence_capacity_bytes") > 0
            && (expect_compact_h32_direct
                ? fragment_native(
                        "fragment_native_peak_query_plan_capacity_bytes") > 0
                : fragment_native(
                        "fragment_native_peak_query_plan_capacity_bytes") == 0)
            && fragment_native(
                    "fragment_native_query_plan_allocations")
                == (expect_compact_h32_direct ? 1 : 0)
            && (expect_compact_h32_direct
                ? fragment_native(
                        "fragment_native_peak_query_group_offset_capacity_bytes")
                        == 0
                    && fragment_native(
                        "fragment_native_query_group_offset_allocations") == 0
                    && fragment_native(
                        "fragment_native_peak_query_membership_filter_capacity_bytes")
                        == 0
                    && fragment_native(
                        "fragment_native_query_membership_filter_allocations")
                        == 0
                : fragment_native(
                        "fragment_native_peak_query_group_offset_capacity_bytes")
                        > 0
                    && static_cast<__int128>(fragment_native(
                        "fragment_native_peak_query_group_offset_capacity_bytes"))
                        <= (static_cast<__int128>(fragment_native(
                            "fragment_native_unique_query_plans")) + 1)
                            * static_cast<__int128>(sizeof(std::uint32_t))
                    && fragment_native(
                        "fragment_native_query_group_offset_allocations") > 0
                    && fragment_native(
                        "fragment_native_peak_query_membership_filter_capacity_bytes")
                        == 16 * 1024 * 1024
                    && fragment_native(
                        "fragment_native_query_membership_filter_allocations")
                        > 0)
            && (expect_compact_h32_direct
                ? fragment_native(
                        "fragment_native_peak_query_radix_capacity_bytes") == 0
                : fragment_native(
                        "fragment_native_peak_query_radix_capacity_bytes") > 0)
            && (expect_compact_h32_direct
                ? fragment_native(
                        "fragment_native_peak_occurrence_count_capacity_bytes")
                        == 0
                : fragment_native(
                        "fragment_native_peak_occurrence_count_capacity_bytes")
                        > 0)
            && static_cast<__int128>(fragment_native(
                    "fragment_native_peak_occurrence_count_capacity_bytes"))
                <= static_cast<__int128>(fragment_native(
                    "fragment_native_unique_query_plans"))
                    * static_cast<__int128>(
                        sizeof(std::atomic<std::uint32_t>))
            && fragment_native(
                    "fragment_native_peak_block_layout_capacity_bytes") > 0
            && fragment_native(
                    "fragment_native_peak_block_evidence_capacity_bytes") > 0
            && !out.format5_reader_memory_evidence[
                "fragment_native_evidence_saturated"].get<bool>()
        : no_fragment_native_query_evidence;
    const bool format5_reader_memory_gate = expect_format5 && !expect_gpu
        ? out.format5_reader_global_position_storage_preparations == 0
            && out.format5_reader_global_position_storage_allocations == 0
            && out.format5_reader_position_append_growth_reservations == 0
            && out.format5_reader_retained_position_count == 0
            && out.format5_reader_retained_position_capacity == 0
            && out.format5_reader_global_position_capacity_bytes == 0
            && format5_reader_exact_position_bytes == 0
            && out.format5_reader_exact_position_image_checks
                == out.persisted_generation_read_sessions
            && out.format5_reader_exact_position_image_failures == 0
            && out.format5_reader_retained_layout_checks
                == out.persisted_generation_read_sessions
            && out.format5_reader_retained_layout_failures == 0
            && format5_reader_layout_bytes
                == static_cast<__int128>(
                    out.format5_reader_owned_capacity_bytes)
            && out.format5_reader_owned_capacity_bytes
                == out.format5_reader_admitted_bytes
            && (expect_compact_h32_direct
                ? out.format5_reader_directory_capacity_bytes == 0
                : out.format5_reader_directory_capacity_bytes > 0)
            && out.format5_reader_query_record_capacity_bytes == 0
            && (expect_compact_h32_direct
                ? out.format5_reader_ordinal_route_capacity_bytes == 0
                : out.format5_reader_ordinal_route_capacity_bytes > 0)
            && out.format5_reader_selected_extent_capacity_bytes == 0
            && out.format5_reader_query_hash_capacity_bytes == 0
            && out.format5_reader_current_selected_control_capacity_bytes == 0
            && out.format5_reader_control_payload_capacity_bytes == 0
            && format5_reader_accounting_shape
            && fragment_native_query_gate
            && out.format5_reader_memory_evidence_saturated == 0
        : no_format5_reader_memory_evidence
            && fragment_native_query_gate;
    // Format 5 is consumed directly from the authenticated persisted reader.
    // That path prepares a key set and matching reader checksum, but it does
    // not construct or reuse the legacy in-memory target-index hashmap.
    const bool cpu_prepared_gate = expect_persisted && !expect_gpu
        ? out.cpu_prepared_index_builds == 0
            && out.cpu_prepared_index_reuses == 0
            && out.cpu_prepared_unique_keys > 0
			&& (!expect_compact_h32_direct
				|| out.cpu_prepared_unique_keys
					== out.fragment_native_unique_query_plans)
            && out.cpu_prepared_duplicate_build_attempts == 0
            && fixed_lower_hex(out.cpu_prepared_key_set_checksum, 16)
            && fixed_lower_hex(out.cpu_prepared_build_set_checksum, 16)
            && out.cpu_prepared_key_set_checksum
                == out.cpu_prepared_build_set_checksum
        : out.cpu_prepared_index_builds == 0
            && out.cpu_prepared_index_reuses == 0
            && out.cpu_prepared_unique_keys == 0
            && out.cpu_prepared_duplicate_build_attempts == 0
            && out.cpu_prepared_key_set_checksum.empty()
            && out.cpu_prepared_build_set_checksum.empty();
    const bool gpu_residency_gate = expect_persisted && expect_gpu
        ? out.gpu_persisted_resident_uploads == 1
            && out.gpu_persisted_resident_cache_hits
                == expectation.attempted_alignment_jobs - 1
        : out.gpu_persisted_resident_uploads == 0
            && out.gpu_persisted_resident_cache_hits == 0;
    const bool simd_json_counters_match_prefix =
        out.simd_evidence_schema_valid == 1
        && out.nthash_vector_batches == counter(94);
    const bool no_block_aligner_execution_evidence =
        out.requested_simd_alignment_algorithm == "none"
        && out.observed_simd_alignment_algorithm == "none"
        && out.block_aligner_execution_proof_present == 0
        && out.block_aligner_execution_proof_accepted == 0
        && out.block_aligner_executed_calls == 0
        && out.block_aligner_nonempty_calls == 0
        && out.block_aligner_nonempty_calls_with_simd == 0
        && out.block_aligner_simd_dp_vector_batches == 0
        && out.block_aligner_simd_dp_cells == 0
        && out.block_aligner_forbidden_ksw2_calls == 0
        && out.block_aligner_forbidden_scalar_biological_fallbacks == 0
        && out.block_aligner_execution_proof_saturated == 0;
    const bool accepted_block_aligner_execution_evidence =
        out.requested_simd_alignment_algorithm == "block_aligner"
        && out.observed_simd_alignment_algorithm == "block_aligner"
        && out.block_aligner_execution_proof_present == 1
        && out.block_aligner_execution_proof_accepted == 1
        && out.block_aligner_executed_calls > 0
        && out.block_aligner_nonempty_calls > 0
        && out.block_aligner_nonempty_calls_with_simd
            == out.block_aligner_nonempty_calls
        && out.block_aligner_simd_dp_vector_batches > 0
        && out.block_aligner_simd_dp_cells > 0
        && out.block_aligner_forbidden_ksw2_calls == 0
        && out.block_aligner_forbidden_scalar_biological_fallbacks == 0
        && out.block_aligner_execution_proof_saturated == 0;
    const bool no_simd_policy_evidence =
        simd_json_counters_match_prefix
        && out.requested_simd_chaining_policy == "none"
        && out.observed_simd_chaining_policy == "none"
        && out.nthash_vector_batches == 0
        && out.nthash_resident_vector_stream_kernel_calls == 0
        && out.simd_chaining_exact_policy_calls == 0
        && out.simd_chaining_bounded_policy_calls == 0
        && out.simd_chaining_bounded_score_batches == 0
        && out.simd_chaining_bounded_cutoff_stops == 0
        && out.simd_chaining_bounded_candidates_skipped == 0
        && out.simd_chaining_policy_rejections == 0
        && out.simd_chaining_policy_evidence_saturated == 0
        && no_block_aligner_execution_evidence;
    const bool bounded_simd_policy_evidence =
        simd_json_counters_match_prefix
        && out.requested_simd_chaining_policy
            == "bounded_batch_approximate"
        && out.observed_simd_chaining_policy
            == "bounded_batch_approximate"
        && (out.nthash_vector_batches > 0
            || out.nthash_resident_vector_stream_kernel_calls > 0)
        && out.simd_chaining_exact_policy_calls == 0
        && out.simd_chaining_bounded_policy_calls > 0
        && out.simd_chaining_bounded_score_batches > 0
        && out.simd_chaining_policy_rejections == 0
        && out.simd_chaining_policy_evidence_saturated == 0
        && accepted_block_aligner_execution_evidence;
    const bool simd_operational_gate = !expect_simd
        || (bounded_simd_policy_evidence
            && counter(93) == 0
            && counter(95) == 0
            && counter(96) == 0
            && counter(98) > 0
            && counter(99) > 0
            && counter(100) == 0
            && counter(101) == 0
            && std::all_of(
                out.alignment_snapshot_numeric_columns.begin() + 103,
                out.alignment_snapshot_numeric_columns.begin() + 139,
                [](const std::int64_t value) noexcept { return value == 0; }));
    const bool scalar_operational_gate = !expect_scalar
        || (no_simd_policy_evidence
            && std::all_of(
                out.alignment_snapshot_numeric_columns.begin() + 94,
                out.alignment_snapshot_numeric_columns.begin() + 102,
                [](const std::int64_t value) noexcept { return value == 0; })
            && std::all_of(
                out.alignment_snapshot_numeric_columns.begin() + 103,
                out.alignment_snapshot_numeric_columns.begin() + 139,
                [](const std::int64_t value) noexcept { return value == 0; }));
    const bool gpu_cache_identity_gate = !expect_gpu
        || gpu_cache_identity_evidence_valid({
            .device_uuid = out.gpu_device_uuid,
            .driver = out.gpu_driver,
            .policy = out.gpu_pipeline_cache_policy,
            .seed_sha256 = out.gpu_pipeline_cache_seed_sha256,
            .seed_size = out.gpu_pipeline_cache_seed_size,
            .manifest_checksum = out.gpu_pipeline_cache_manifest_checksum,
            .hit = out.gpu_pipeline_cache_hit,
        });
    const bool gpu_cache_runtime_gate = !expect_gpu
        || gpu_cache_runtime_evidence_valid({
            .declared_seed_size = out.gpu_pipeline_cache_seed_size,
            .loads = counter(124),
            .seed_bytes = counter(125),
            .load_ns = counter(126),
            .creation_ns = counter(127),
            .timestamp_failures = counter(133),
            .pipeline_creates = counter(138),
            .backend_declines = out.backend_declines,
            .backend_fallbacks = out.backend_fallbacks,
        });
    const bool gpu_operational_gate = !expect_gpu
        || (no_simd_policy_evidence
            && gpu_cache_identity_gate
            && gpu_cache_runtime_gate
            && counter(82) == 0
            && counter(84) == 0
            && counter(85) == 0
            && counter(86) == 0
            && counter(87) == 0
            && counter(88) == 0
            && counter(89) == 0
            && counter(90) == 0
            && counter(91) == 0
            && counter(92) == 0
            && counter(93) == 0
            && std::all_of(
                out.alignment_snapshot_numeric_columns.begin() + 94,
                out.alignment_snapshot_numeric_columns.begin() + 103,
                [](const std::int64_t value) noexcept { return value == 0; })
            && counter(112) == 0);
    const bool persisted_gpu_operation_gate =
        !expect_gpu || !expect_persisted
        || (out.persisted_gpu_operation_evidence_schema_valid == 1
            && out.persisted_gpu_operation_evidence_accepted == 1
            && out.persisted_gpu_operation_evidence_complete == 1
            && out.persisted_gpu_operation_evidence_saturated == 0
            && out.persisted_gpu_dropped_operation_count == 0
            && out.persisted_gpu_operation_count > 0
            && out.persisted_gpu_accepted_operation_count
                == out.persisted_gpu_operation_count
            && out.persisted_gpu_job_count
                == expectation.attempted_alignment_jobs
            && out.persisted_gpu_accepted_job_count
                == out.persisted_gpu_job_count);
    const bool cold_restart_process_gate = !expect_persisted
        || (out.just_index_restart_performed == 1
            && out.cold_restart_identity_changed == 1
            && !out.pre_restart_postmaster_start_time.empty()
            && !out.post_restart_postmaster_start_time.empty()
            && out.pre_restart_postmaster_start_time
                != out.post_restart_postmaster_start_time
            && out.timed_postmaster_start_time
                == out.post_restart_postmaster_start_time
            && out.pre_restart_backend_pid > 0
            && out.post_restart_backend_pid > 0
            && out.pre_restart_backend_pid != out.post_restart_backend_pid
            && out.pre_restart_database_oid > 0
            && out.pre_restart_database_oid == out.post_restart_database_oid
            && out.post_restart_backend_pid == out.alignment_backend_pid
            && out.post_restart_database_oid == out.alignment_database_oid
            && out.pre_restart_persisted_generation_id > 0
            && out.pre_restart_persisted_generation_id
                == out.persisted_generation_id);
    const bool format5_cold_restart_identity_gate = !expect_format5
        || (out.pre_restart_persisted_format_version == 5
            && out.post_restart_persisted_format_version == 5
            && fixed_lower_hex(
                out.pre_restart_persisted_source_identity, 16)
            && out.pre_restart_persisted_source_identity
                != "0000000000000000"
            && out.pre_restart_persisted_source_identity
                == out.persisted_source_identity);
    const bool format6_cold_restart_identity_gate = !expect_format6
        || (out.pre_restart_persisted_format_version == 6
            && out.post_restart_persisted_format_version == 6
            && out.format6_restart_audit_accepted == 1
            && out.pre_restart_format6_ready_audit
                == out.post_restart_format6_ready_audit
            && fixed_lower_hex(
                out.pre_restart_persisted_source_identity, 32)
            && required_json_text(
                out.format6_reader_evidence, "source_identity")
                == out.pre_restart_persisted_source_identity);
    const bool cold_restart_gate = cold_restart_process_gate
        && format5_cold_restart_identity_gate
        && format6_cold_restart_identity_gate;
    const bool cold_no_index_gate = expect_persisted
        || out.cold_no_index_lifecycle_accepted == 1;
    const bool persisted_policy_gate = !expect_persisted
        || out.persisted_policy_audit_accepted == 1;
    const bool proof_envelope_gate =
        out.pipeline_proof_present == 1
        && out.pipeline_proof_attached_to_measurement == 1
        && out.stage_engine_matrix_valid == 1
        && out.alignment_execution_id > 0
        && out.alignment_backend_pid > 0
        && out.alignment_database_oid > 0
        && out.effective_cpu_workers == expectation.effective_cpu_workers
        && out.alignment_scan_node_count == 1
        && out.nested_alignment_executor_count == 0
        && out.alignment_completed == 1
        && out.alignment_partial == 0
        && out.alignment_fully_drained == 1
        && out.stats_saturated == 0
        && out.cold_index_stats_saturated == out.stats_saturated
        && out.backend_declines == 0
        && out.backend_fallbacks == 0;
    const bool attested_cardinality_gate =
        expectation.attempted_alignment_jobs > 0
        && exact_cardinality
        && gpu_signoff_shape
        && out.fully_attested_alignment_jobs
            == out.attempted_alignment_jobs
        && out.unattested_alignment_jobs == 0;
    const bool backend_identity_gate =
        out.requested_backend == expected_backend
        && out.actual_backend == expected_backend
        && out.requested_target_index_source == expected_source
        && out.actual_target_index_source == expected_source
        && out.environment_mixed_invalid == 0
        && out.mixed_persisted_identity == 0
        && out.operational_identity_conflict == 0;
    const bool accepted =
        proof_envelope_gate
        && attested_cardinality_gate
        && backend_identity_gate
        && row_equation
        && source_valid
        && persisted_identity_valid
        && persisted_read_gate
        && format5_reader_memory_gate
        && cpu_prepared_gate
        && gpu_residency_gate
        && simd_operational_gate
        && scalar_operational_gate
        && gpu_operational_gate
        && persisted_gpu_operation_gate
        && cold_restart_gate
        && cold_no_index_gate
        && persisted_policy_gate
        && backend_specific;
    out.pipeline_proof_accepted = accepted ? 1 : 0;
    if (accepted) {
        return;
    }
    std::fprintf(
        stderr,
        "[bench_e2e] PIPELINE-PROOF-FAIL algo=%.*s present=%lld attached=%lld "
        "stage_matrix=%lld "
        "requested_backend=%s actual_backend=%s requested_source=%s "
        "actual_source=%s attempted=%lld fully_attested=%lld unattested=%lld "
        "targets=%lld expected_targets=%lld expected_attempted=%lld "
        "targets_per_dispatch=%lld workers=%lld expected_workers=%lld "
        "persisted=%lld missing=%lld recomputed=%lld "
        "prepared_builds=%lld prepared_reuses=%lld prepared_unique=%lld "
        "prepared_duplicates=%lld generation_reads=%lld relation_opens=%lld "
        "index_scans=%lld gpu_uploads=%lld gpu_hits=%lld restart_changed=%lld "
        "persisted_policy_accepted=%lld "
        "declines=%lld fallbacks=%lld\n",
        static_cast<int>(algo.size()), algo.data(),
        static_cast<long long>(out.pipeline_proof_present),
        static_cast<long long>(out.pipeline_proof_attached_to_measurement),
        static_cast<long long>(out.stage_engine_matrix_valid),
        out.requested_backend.c_str(),
        out.actual_backend.c_str(),
        out.requested_target_index_source.c_str(),
        out.actual_target_index_source.c_str(),
        static_cast<long long>(out.attempted_alignment_jobs),
        static_cast<long long>(out.fully_attested_alignment_jobs),
        static_cast<long long>(out.unattested_alignment_jobs),
        static_cast<long long>(out.target_rows),
        static_cast<long long>(expectation.target_rows),
        static_cast<long long>(expectation.attempted_alignment_jobs),
        static_cast<long long>(expectation.target_rows_per_dispatch),
        static_cast<long long>(out.effective_cpu_workers),
        static_cast<long long>(expectation.effective_cpu_workers),
        static_cast<long long>(out.persisted_index_rows),
        static_cast<long long>(out.missing_persisted_index_rows),
        static_cast<long long>(out.recomputed_target_index_rows),
        static_cast<long long>(out.cpu_prepared_index_builds),
        static_cast<long long>(out.cpu_prepared_index_reuses),
        static_cast<long long>(out.cpu_prepared_unique_keys),
        static_cast<long long>(out.cpu_prepared_duplicate_build_attempts),
        static_cast<long long>(out.persisted_generation_read_sessions),
        static_cast<long long>(out.persisted_relation_opens),
        static_cast<long long>(out.persisted_index_scans),
        static_cast<long long>(out.gpu_persisted_resident_uploads),
        static_cast<long long>(out.gpu_persisted_resident_cache_hits),
        static_cast<long long>(out.cold_restart_identity_changed),
        static_cast<long long>(out.persisted_policy_audit_accepted),
        static_cast<long long>(out.backend_declines),
        static_cast<long long>(out.backend_fallbacks));
    std::fprintf(
        stderr,
        "[bench_e2e] PIPELINE-PROOF-FAIL-GATES "
        "proof_envelope=%d attested_cardinality=%d backend_identity=%d "
        "row_equation=%d source_valid=%d persisted_identity=%d "
        "persisted_read=%d format5_reader_memory=%d cpu_prepared=%d "
        "gpu_residency=%d "
        "simd_operational=%d scalar_operational=%d gpu_operational=%d "
        "persisted_gpu_operation=%d "
        "gpu_cache_identity=%d gpu_cache_runtime=%d "
        "cold_restart=%d cold_no_index=%d persisted_policy=%d "
        "backend_specific=%d manifest_size=%d cold_payload_equations=%d "
        "one_cold_route=%d selective_read=%d full_read=%d "
        "prepared_key_checksum=%s prepared_reader_checksum=%s\n",
        static_cast<int>(proof_envelope_gate),
        static_cast<int>(attested_cardinality_gate),
        static_cast<int>(backend_identity_gate),
        static_cast<int>(row_equation),
        static_cast<int>(source_valid),
        static_cast<int>(persisted_identity_valid),
        static_cast<int>(persisted_read_gate),
        static_cast<int>(format5_reader_memory_gate),
        static_cast<int>(cpu_prepared_gate),
        static_cast<int>(gpu_residency_gate),
        static_cast<int>(simd_operational_gate),
        static_cast<int>(scalar_operational_gate),
        static_cast<int>(gpu_operational_gate),
        static_cast<int>(persisted_gpu_operation_gate),
        static_cast<int>(gpu_cache_identity_gate),
        static_cast<int>(gpu_cache_runtime_gate),
        static_cast<int>(cold_restart_gate),
        static_cast<int>(cold_no_index_gate),
        static_cast<int>(persisted_policy_gate),
        static_cast<int>(backend_specific),
        static_cast<int>(persisted_manifest_size_valid),
        static_cast<int>(cold_payload_equations),
        static_cast<int>(one_cold_route),
        static_cast<int>(selective_read_gate),
        static_cast<int>(full_read_gate),
        out.cpu_prepared_key_set_checksum.c_str(),
        out.cpu_prepared_build_set_checksum.c_str());
    if (expect_gpu) {
        std::fprintf(
            stderr,
            "[bench_e2e] PIPELINE-PROOF-FAIL-GPU-CACHE "
            "identity=%d runtime=%d uuid=%s driver=%s policy=%s "
            "seed_sha256=%s seed_size=%lld manifest=%s hit=%lld "
            "loads=%lld seed_bytes=%lld load_ns=%lld creation_ns=%lld "
            "timestamp_failures=%lld pipeline_creates=%lld "
            "declines=%lld fallbacks=%lld\n",
            static_cast<int>(gpu_cache_identity_gate),
            static_cast<int>(gpu_cache_runtime_gate),
            out.gpu_device_uuid.c_str(),
            out.gpu_driver.c_str(),
            out.gpu_pipeline_cache_policy.c_str(),
            out.gpu_pipeline_cache_seed_sha256.c_str(),
            static_cast<long long>(out.gpu_pipeline_cache_seed_size),
            out.gpu_pipeline_cache_manifest_checksum.c_str(),
            static_cast<long long>(out.gpu_pipeline_cache_hit),
            static_cast<long long>(counter(124)),
            static_cast<long long>(counter(125)),
            static_cast<long long>(counter(126)),
            static_cast<long long>(counter(127)),
            static_cast<long long>(counter(133)),
            static_cast<long long>(counter(138)),
            static_cast<long long>(out.backend_declines),
            static_cast<long long>(out.backend_fallbacks));
        std::fprintf(
            stderr,
            "[bench_e2e] PIPELINE-PROOF-FAIL-GPU-OPERATION "
            "gate=%d schema=%lld accepted=%lld complete=%lld "
            "saturated=%lld dropped=%lld operations=%lld/%lld "
            "jobs=%lld/%lld expected_jobs=%lld\n",
            static_cast<int>(persisted_gpu_operation_gate),
            static_cast<long long>(
                out.persisted_gpu_operation_evidence_schema_valid),
            static_cast<long long>(
                out.persisted_gpu_operation_evidence_accepted),
            static_cast<long long>(
                out.persisted_gpu_operation_evidence_complete),
            static_cast<long long>(
                out.persisted_gpu_operation_evidence_saturated),
            static_cast<long long>(
                out.persisted_gpu_dropped_operation_count),
            static_cast<long long>(out.persisted_gpu_accepted_operation_count),
            static_cast<long long>(out.persisted_gpu_operation_count),
            static_cast<long long>(out.persisted_gpu_accepted_job_count),
            static_cast<long long>(out.persisted_gpu_job_count),
            static_cast<long long>(expectation.attempted_alignment_jobs));
    }
    if (expect_persisted) {
        std::fprintf(
            stderr,
            "[bench_e2e] PIPELINE-PROOF-FAIL-PERSISTED-READ "
            "route=%d session=%d head=%d manifest=%d directory=%d "
            "payload=%d scan_equations=%d zero_work=%d "
            "cold_schema=%lld pre_restart_gpu_abi=%lld "
            "format5_contract=%lld target_rows=%lld "
            "memory_passes=%lld memory_preparations=%lld "
            "memory_allocations=%lld append_growth=%lld retained=%lld/%lld "
            "position_bytes=%lld layout=%lld/%lld admitted=%lld "
            "exact_checks=%lld/%lld layout_checks=%lld/%lld "
            "memory_saturated=%lld\n",
            static_cast<int>(persisted_read_route_proof),
            static_cast<int>(persisted_read_session_proof),
            static_cast<int>(persisted_read_head_proof),
            static_cast<int>(persisted_read_manifest_proof),
            static_cast<int>(persisted_read_directory_proof),
            static_cast<int>(persisted_read_payload_proof),
            static_cast<int>(persisted_read_scan_equations),
            static_cast<int>(persisted_read_zero_work_proof),
            static_cast<long long>(out.cold_index_schema_valid),
            static_cast<long long>(out.pre_restart_persisted_gpu_abi_version),
            static_cast<long long>(
                out.persisted_format5_manifest_contract_valid),
            static_cast<long long>(out.target_rows),
            static_cast<long long>(
                out.format5_reader_capacity_accounting_passes),
            static_cast<long long>(
                out.format5_reader_global_position_storage_preparations),
            static_cast<long long>(
                out.format5_reader_global_position_storage_allocations),
            static_cast<long long>(
                out.format5_reader_position_append_growth_reservations),
            static_cast<long long>(out.format5_reader_retained_position_count),
            static_cast<long long>(out.format5_reader_retained_position_capacity),
            static_cast<long long>(
                out.format5_reader_global_position_capacity_bytes),
            static_cast<long long>(out.format5_reader_owned_capacity_bytes),
            static_cast<long long>(format5_reader_layout_bytes),
            static_cast<long long>(out.format5_reader_admitted_bytes),
            static_cast<long long>(
                out.format5_reader_exact_position_image_checks),
            static_cast<long long>(
                out.format5_reader_exact_position_image_failures),
            static_cast<long long>(out.format5_reader_retained_layout_checks),
            static_cast<long long>(out.format5_reader_retained_layout_failures),
            static_cast<long long>(
                out.format5_reader_memory_evidence_saturated));
        std::fputs(
            "[bench_e2e] PIPELINE-PROOF-FAIL-PERSISTED-COUNTERS",
            stderr);
        for (std::size_t column = 48; column <= 85; ++column) {
            const std::string_view name =
                kAlignmentSnapshotColumnNames[column];
            std::fprintf(
                stderr, " %.*s=%lld",
                static_cast<int>(name.size()), name.data(),
                static_cast<long long>(counter(column)));
        }
        std::fputc('\n', stderr);
    }
    std::fprintf(
        stderr,
        "[bench_e2e] PIPELINE-PROOF-FAIL-SIMD "
        "requested_policy=%s observed_policy=%s "
        "requested_alignment=%s observed_alignment=%s "
        "json_matches_prefix=%d no_simd_evidence=%d bounded_simd_evidence=%d "
        "no_block_evidence=%d accepted_block_evidence=%d "
        "block_present=%lld block_accepted=%lld block_calls=%lld "
        "block_nonempty=%lld block_nonempty_simd=%lld "
        "block_vector_batches=%lld block_cells=%lld block_ksw2=%lld "
        "block_scalar_fallbacks=%lld block_saturated=%lld\n",
        out.requested_simd_chaining_policy.c_str(),
        out.observed_simd_chaining_policy.c_str(),
        out.requested_simd_alignment_algorithm.c_str(),
        out.observed_simd_alignment_algorithm.c_str(),
        static_cast<int>(simd_json_counters_match_prefix),
        static_cast<int>(no_simd_policy_evidence),
        static_cast<int>(bounded_simd_policy_evidence),
        static_cast<int>(no_block_aligner_execution_evidence),
        static_cast<int>(accepted_block_aligner_execution_evidence),
        static_cast<long long>(out.block_aligner_execution_proof_present),
        static_cast<long long>(out.block_aligner_execution_proof_accepted),
        static_cast<long long>(out.block_aligner_executed_calls),
        static_cast<long long>(out.block_aligner_nonempty_calls),
        static_cast<long long>(out.block_aligner_nonempty_calls_with_simd),
        static_cast<long long>(out.block_aligner_simd_dp_vector_batches),
        static_cast<long long>(out.block_aligner_simd_dp_cells),
        static_cast<long long>(out.block_aligner_forbidden_ksw2_calls),
        static_cast<long long>(
            out.block_aligner_forbidden_scalar_biological_fallbacks),
        static_cast<long long>(out.block_aligner_execution_proof_saturated));
    std::fflush(stderr);
    std::abort();
}

// create_e2e_table: a USING bioseq table matching the active alphabet and the
// E2E generator's uint64 storage width.
void disable_e2e_autovacuum(const it::connection_test_context& c,
                            const std::string& table) {
    it::exec_sqlf(c, "ALTER TABLE {} SET (autovacuum_enabled = false);", table);
}

void create_e2e_table(it::connection_test_context& c, const std::string& t,
                      const std::string& seq_type) {
    it::exec_sqlf(c, "DROP TABLE IF EXISTS {} CASCADE;", t);
    it::exec_sqlf(c, "CREATE TABLE {} (header text, seq {}) USING bioseq;",
                  t, seq_type);
    disable_e2e_autovacuum(c, t);
}

void configure_bioseq_aligner_session(it::connection_test_context& c,
                                      int kmer_size,
                                      int window_size) {
    it::exec_sql(c, "SET bioseq.aligner.base_alignment_algorithm = 'block_aligner';");
    it::exec_sql(c, "SET bioseq.aligner.chaining_algorithm       = 'simd_mm2';");
    it::exec_sql(c, "SET bioseq.aligner.minimizer_algorithm      = 'two_stack_nthash';");
    it::exec_sqlf(c, "SET bioseq.aligner.kmer_size   = {};", kmer_size);
    it::exec_sqlf(c, "SET bioseq.aligner.window_size = {};", window_size);
}

void configure_bioseq_simd_session(
    it::connection_test_context& connection,
    int kmer_size,
    int window_size)
{
    configure_bioseq_aligner_session(connection, kmer_size, window_size);
    it::exec_sql(connection, "SET bioseq.aligner.vulkan_enabled = off;");
    it::exec_sql(connection, "SET bioseq.aligner.vulkan_seed_hits_enabled = off;");
    it::exec_sql(connection, "SET bioseq.aligner.vulkan_index_cache_enabled = off;");
    it::exec_sql(connection, "SET bioseq.aligner.vulkan_fused_frontend_enabled = off;");
    it::exec_sql(connection, "SET bioseq.aligner.vulkan_gpu_index_build_enabled = off;");
    it::exec_sql(connection, "SET bioseq.aligner.vulkan_extend_enabled = off;");
}

void configure_bioseq_scalar_session(
    it::connection_test_context& connection,
    int kmer_size,
    int window_size)
{
    configure_bioseq_aligner_session(connection, kmer_size, window_size);
    it::exec_sql(
        connection,
        "SET bioseq.aligner.base_alignment_algorithm = 'ksw2';");
    it::exec_sql(
        connection,
        "SET bioseq.aligner.chaining_algorithm = 'scalar_mm2';");
    it::exec_sql(
        connection,
        "SET bioseq.aligner.minimizer_algorithm = 'scalar';");
    it::exec_sql(connection, "SET bioseq.aligner.vulkan_enabled = off;");
    it::exec_sql(connection, "SET bioseq.aligner.vulkan_seed_hits_enabled = off;");
    it::exec_sql(connection, "SET bioseq.aligner.vulkan_index_cache_enabled = off;");
    it::exec_sql(connection, "SET bioseq.aligner.vulkan_fused_frontend_enabled = off;");
    it::exec_sql(connection, "SET bioseq.aligner.vulkan_gpu_index_build_enabled = off;");
    it::exec_sql(connection, "SET bioseq.aligner.vulkan_extend_enabled = off;");
}

// DOC0345 Patch L — full Vulkan minimizer + GPU-index session config. Applied
// BEFORE any persisted index build so the untimed just_index build runs on the
// GPU (vulkan_minimap_int + vulkan_gpu_index_build_enabled), not CPU/SIMD.
void configure_bioseq_vulkan_index_session(
    it::connection_test_context& connection,
    int kmer_size,
    int window_size,
    bool enable_extension = false)
{
    configure_bioseq_aligner_session(connection, kmer_size, window_size);
    it::exec_sql(
        connection,
        "SET bioseq.aligner.chaining_algorithm = 'vulkan_tiled_dp';");
    it::exec_sql(
        connection,
        "SET bioseq.aligner.minimizer_algorithm = 'vulkan_minimap_int';");
    it::exec_sql(
        connection,
        "SET bioseq.aligner.vulkan_enabled = on;");
    it::exec_sql(
        connection,
        "SET bioseq.aligner.vulkan_seed_hits_enabled = on;");
    it::exec_sql(
        connection,
        "SET bioseq.aligner.vulkan_index_cache_enabled = on;");
    it::exec_sql(
        connection,
        "SET bioseq.aligner.vulkan_fused_frontend_enabled = on;");
    it::exec_sql(
        connection,
        "SET bioseq.aligner.vulkan_gpu_index_build_enabled = on;");
    it::exec_sqlf(
        connection,
        "SET bioseq.aligner.vulkan_extend_enabled = {};",
        enable_extension ? "on" : "off");
}

[[nodiscard]] std::string param_string(const ParamMap& params,
                                       std::string_view name,
                                       std::string_view fallback) {
    const auto it = params.find(std::string{name});
    if (it == params.end() || !it->second.is_string()) {
        return std::string{fallback};
    }
    return std::string{it->second.as_string()};
}

[[nodiscard]] int param_int(const ParamMap& params,
                            std::string_view name,
                            int fallback) {
    const auto it = params.find(std::string{name});
    if (it == params.end() || !it->second.is_integer()) {
        return fallback;
    }
    return static_cast<int>(it->second.as_integer());
}

[[nodiscard]] int fair_scheduler_worker_count(
        const it::connection_test_context& connection) {
    const std::int64_t workers = it::query_scalar_int(
        connection,
        "SELECT max_threads::bigint FROM bioseq.scheduler_stats();");
    if (workers <= 0
        || workers > static_cast<std::int64_t>(
            (std::numeric_limits<int>::max)())) {
        std::fprintf(
            stderr,
            "[bench_e2e] INVALID-SCHEDULER-WORKER-COUNT workers=%lld\n",
            static_cast<long long>(workers));
        std::fflush(stderr);
        std::abort();
    }
    return static_cast<int>(workers);
}

[[nodiscard]] std::string param_minimizer_index(const ParamMap& params) {
    std::string index =
        param_string(params, "minimizer_index", kMinimizersPerFragmentIndex);
    if (index != kMinimizersPerFragmentIndex
        && index != kMinimizersPerSequenceIndex) {
        bioseq_log<ERR>("[bench_e2e] unsupported minimizer_index '{}'", index);
        std::abort();
    }
    return index;
}

[[nodiscard]] std::string param_mps_pair_sort_impl(const ParamMap& params) {
    std::string impl = param_string(params, "mps_pair_sort_impl", "std_sort");
    if (impl != "std_sort"
        && impl != "simd_u128_vqsort"
        && impl != "simd_u128_vec_stream_1"
        && impl != "simd_u128_vec_stream_2"
        && impl != "simd_u128_vec_stream_4") {
        bioseq_log<ERR>("[bench_e2e] unsupported mps_pair_sort_impl '{}'", impl);
        std::abort();
    }
    return impl;
}

void configure_bioseq_pair_sort_session(
        it::connection_test_context& c,
        const ParamMap& params) {
    const std::string impl = param_mps_pair_sort_impl(params);
    it::exec_sqlf(c, "SET bioseq.mps_pair_sort_impl = '{}';", impl);
}

void configure_bioseq_scheduler_session(it::connection_test_context& c,
                                        std::string_view pipeline_mode) {
    it::exec_sql(c, "SET max_parallel_workers_per_gather = 0;");
    it::exec_sqlf(c, "SET bioseq.scheduler.pipeline_concurrency_mode = '{}';",
                  pipeline_mode);
    it::exec_sql(c, "SET bioseq.index_async_persist = on;");
}

void configure_bioseq_minimizer_policy(
        it::connection_test_context& c,
        const std::string& table,
        std::string_view minimizer_index) {
    if (minimizer_index == kMinimizersPerSequenceIndex) {
        it::exec_sqlf(c,
            "SELECT bioseq.set_index_policy('{}'::regclass, '{}', 'UPDATE_MANUALLY');",
            table, kMinimizersPerFragmentIndex);
        it::exec_sqlf(c,
            "SELECT bioseq.set_index_policy('{}'::regclass, '{}', 'UPDATE_ON_INSERT');",
            table, kMinimizersPerSequenceIndex);
        it::exec_sqlf(c,
            "SELECT bioseq.set_active_read_index('{}'::regclass, '{}');",
            table, kMinimizersPerSequenceIndex);
    } else {
        it::exec_sqlf(c,
            "SELECT bioseq.set_index_policy('{}'::regclass, '{}', 'UPDATE_ON_INSERT');",
            table, kMinimizersPerFragmentIndex);
        it::exec_sqlf(c,
            "SELECT bioseq.set_index_policy('{}'::regclass, '{}', 'UPDATE_MANUALLY');",
            table, kMinimizersPerSequenceIndex);
        it::exec_sqlf(c,
            "SELECT bioseq.set_active_read_index('{}'::regclass, '{}');",
            table, kMinimizersPerFragmentIndex);
    }
}

[[nodiscard]] const std::string& just_index_table_for(
        const E2eInput& in,
        std::string_view minimizer_index,
        JustIndexSetupKind setup_kind = JustIndexSetupKind::Simd) {
    if (minimizer_index == kMinimizersPerSequenceIndex) {
        switch (setup_kind) {
            case JustIndexSetupKind::Scalar:
                return in.just_index_table_scalar_per_sequence;
            case JustIndexSetupKind::Simd:
                return in.just_index_table_simd_per_sequence;
            case JustIndexSetupKind::Gpu:
                return in.just_index_table_gpu_per_sequence;
        }
    }
    switch (setup_kind) {
        case JustIndexSetupKind::Scalar:
            return in.just_index_table_scalar_per_fragment;
        case JustIndexSetupKind::Simd:
            return in.just_index_table_simd_per_fragment;
        case JustIndexSetupKind::Gpu:
            return in.just_index_table_gpu_per_fragment;
    }
    std::abort();
}

[[nodiscard]] std::int64_t first_i64_or_minus_one(const it::rowset_t& rows) {
    if (rows.empty() || rows.front().empty() || !rows.front().front().has_value()) {
        return -1;
    }
    return parse_i64(*rows.front().front());
}

[[nodiscard]] std::int64_t row_i64_or_minus_one(
        const it::row_t& row,
        std::size_t col) {
    if (col >= row.size() || !row[col].has_value()) {
        return -1;
    }
    return parse_i64(*row[col]);
}

[[nodiscard]] std::int64_t capture_only_sequence_rowno(
        const it::connection_test_context& connection,
        const std::string& table) {
    const auto rows = it::query_rows(
        connection,
        fmt::format(
            "SELECT bioseq.column_row_number('{}'::regclass, ctid) "
            "FROM {} ORDER BY ctid;",
            table,
            table));
    const std::int64_t sequence_rowno = rows.size() == 1
        ? row_i64_or_minus_one(rows.front(), 0)
        : -1;
    if (sequence_rowno > 0) {
        return sequence_rowno;
    }
    std::fprintf(
        stderr,
        "[bench_e2e] READY-generation setup requires exactly one visible "
        "source row with a valid table-AM row number; table=%s rows=%zu "
        "sequence_rowno=%lld\n",
        table.c_str(),
        rows.size(),
        static_cast<long long>(sequence_rowno));
    std::fflush(stderr);
    std::abort();
}

[[nodiscard]] std::int64_t just_index_sequence_rowno_for(
        const E2eInput& in,
        const std::string& table) {
    const auto found = in.just_index_sequence_rowno_by_table.find(table);
    if (found != in.just_index_sequence_rowno_by_table.end()
        && found->second > 0) {
        return found->second;
    }
    std::fprintf(
        stderr,
        "[bench_e2e] missing captured table-AM source row identity for "
        "just_index table=%s\n",
        table.c_str());
    std::fflush(stderr);
    std::abort();
}

void capture_mps_build_stats(
        const it::connection_test_context& c,
        E2eOutput& out) {
    const auto rows = it::query_rows(c,
        "SELECT backend_pid, db_oid, relfilenode, sequence_rowno, "
        "total_run_count, resident_runs, file_runs, resident_run_bytes, "
        "resident_peak_bytes, spilled_run_bytes, postgres_temp_file_bytes, "
        "postgres_temp_file_count, merge_reader_buffer_bytes, merge_pass_count, "
        "spill_count_from_pressure, resident_demotions, position_spill_bytes, "
        "position_spill_count, generation, worker_build_ns, pipeline_mode, "
        "max_concurrency, index_memory_cap_mb, lazy_worker_shareable "
        "FROM bioseq.scheduler_mps_build_stats();");
    if (rows.empty()) {
        return;
    }
    const it::row_t& row = rows.front();
    out.mps_build_backend_pid = row_i64_or_minus_one(row, 0);
    out.mps_build_db_oid = row_i64_or_minus_one(row, 1);
    out.mps_build_relfilenode = row_i64_or_minus_one(row, 2);
    out.mps_build_sequence_rowno = row_i64_or_minus_one(row, 3);
    out.mps_build_total_run_count = row_i64_or_minus_one(row, 4);
    out.mps_build_resident_runs = row_i64_or_minus_one(row, 5);
    out.mps_build_file_runs = row_i64_or_minus_one(row, 6);
    out.mps_build_resident_run_bytes = row_i64_or_minus_one(row, 7);
    out.mps_build_resident_peak_bytes = row_i64_or_minus_one(row, 8);
    out.mps_build_spilled_run_bytes = row_i64_or_minus_one(row, 9);
    out.mps_build_postgres_temp_file_bytes = row_i64_or_minus_one(row, 10);
    out.mps_build_postgres_temp_file_count = row_i64_or_minus_one(row, 11);
    out.mps_build_merge_reader_buffer_bytes = row_i64_or_minus_one(row, 12);
    out.mps_build_merge_pass_count = row_i64_or_minus_one(row, 13);
    out.mps_build_spill_count_from_pressure = row_i64_or_minus_one(row, 14);
    out.mps_build_resident_demotions = row_i64_or_minus_one(row, 15);
    out.mps_build_position_spill_bytes = row_i64_or_minus_one(row, 16);
    out.mps_build_position_spill_count = row_i64_or_minus_one(row, 17);
    out.mps_build_shared_generation = row_i64_or_minus_one(row, 18);
    out.mps_build_worker_build_ns = row_i64_or_minus_one(row, 19);
    out.mps_build_pipeline_mode = row_i64_or_minus_one(row, 20);
    out.mps_build_max_concurrency = row_i64_or_minus_one(row, 21);
    out.mps_build_index_memory_cap_mb = row_i64_or_minus_one(row, 22);
    out.mps_build_lazy_worker_shareable = row_i64_or_minus_one(row, 23);
}

[[nodiscard]] std::int64_t index_row_count(
        const it::connection_test_context& c,
        const std::string& table,
        std::string_view index_name) {
    const auto rows = it::query_rows(c, fmt::format(
        "SELECT bioseq.index_table_row_count('{}'::regclass, '{}');",
        table, index_name));
    return first_i64_or_minus_one(rows);
}

[[nodiscard]] ColdNoIndexPolicyAbsenceEvidence
cold_no_index_policy_absence_evidence(
        const it::connection_test_context& c,
        const std::string& table,
        const std::string_view index_name) {
    const auto rows = it::query_rows(c, fmt::format(
        "SELECT target_rows, target_cardinality_metadata_only::int, "
        "source_parent_rows_read, source_fragment_rows_read, "
        "source_decoded_bp, physical_index_relations, "
        "persisted_index_rows, generation_objects "
        "FROM bioseq.inspect_cold_no_index_absence('{}'::regclass, '{}');",
        table, index_name));
    if (rows.size() != 1 || rows.front().size() != 8) {
        std::fprintf(stderr,
            "[bench_e2e] COLD-NO-INDEX-AUDIT-SHAPE-FAIL table=%s index=%.*s\n",
            table.c_str(), static_cast<int>(index_name.size()),
            index_name.data());
        std::fflush(stderr);
        std::abort();
    }
    const it::row_t& row = rows.front();
    return ColdNoIndexPolicyAbsenceEvidence{
        .target_rows = row_i64_or_minus_one(row, 0),
        .target_cardinality_metadata_only = row_i64_or_minus_one(row, 1),
        .source_parent_rows_read = row_i64_or_minus_one(row, 2),
        .source_fragment_rows_read = row_i64_or_minus_one(row, 3),
        .source_decoded_bp = row_i64_or_minus_one(row, 4),
        .physical_index_relations = row_i64_or_minus_one(row, 5),
        .persisted_index_rows = row_i64_or_minus_one(row, 6),
        .generation_objects = row_i64_or_minus_one(row, 7),
    };
}

[[nodiscard]] std::int64_t cold_no_index_gucs_are_off(
        const it::connection_test_context& c) {
    const auto rows = it::query_rows(
        c,
        "SELECT (current_setting('bioseq.update_index_on_insert') = 'off')::int, "
        "       (current_setting('bioseq.read_sequence_index') = 'off')::int, "
        "       (current_setting('bioseq.index_async_persist') = 'off')::int, "
        "       (current_setting('bioseq.recompute_async_persist') = 'off')::int;");
    if (rows.size() != 1 || rows.front().size() != 4) {
        return 0;
    }
    return row_i64_or_minus_one(rows.front(), 0) == 1
            && row_i64_or_minus_one(rows.front(), 1) == 1
            && row_i64_or_minus_one(rows.front(), 2) == 1
            && row_i64_or_minus_one(rows.front(), 3) == 1
        ? 1 : 0;
}

enum class ColdNoIndexAuditPhase : std::uint8_t {
    pre_restart,
    post_restart,
    measured,
};

void capture_cold_no_index_absence(
        const it::connection_test_context& c,
        const E2eInput& in,
        E2eOutput& out,
        const ColdNoIndexAuditPhase phase) {
    const ColdNoIndexPolicyAbsenceEvidence mpf =
        cold_no_index_policy_absence_evidence(
            c, in.no_index_table, kMinimizersPerFragmentIndex);
    const ColdNoIndexPolicyAbsenceEvidence mps =
        cold_no_index_policy_absence_evidence(
            c, in.no_index_table, kMinimizersPerSequenceIndex);
    const std::int64_t target_rows = mpf.target_rows;
    const std::int64_t mpf_rows = mpf.persisted_index_rows;
    const std::int64_t mps_rows = mps.persisted_index_rows;
    const std::int64_t async_jobs = first_i64_or_minus_one(it::query_rows(
        c,
        "SELECT async_persist_jobs_waiting FROM bioseq.scheduler_stats();"));
    const std::int64_t gucs_off = cold_no_index_gucs_are_off(c);

    if (phase == ColdNoIndexAuditPhase::pre_restart) {
        out.cold_no_index_pre_restart_target_rows = target_rows;
        out.cold_no_index_pre_restart_mpf_rows = mpf_rows;
        out.cold_no_index_pre_restart_mps_rows = mps_rows;
        out.cold_no_index_pre_restart_async_jobs = async_jobs;
        out.cold_no_index_pre_restart_gucs_off = gucs_off;
        out.cold_no_index_pre_restart_mpf = mpf;
        out.cold_no_index_pre_restart_mps = mps;
    } else if (phase == ColdNoIndexAuditPhase::post_restart) {
        out.cold_no_index_post_restart_target_rows = target_rows;
        out.cold_no_index_post_restart_mpf_rows = mpf_rows;
        out.cold_no_index_post_restart_mps_rows = mps_rows;
        out.cold_no_index_post_restart_async_jobs = async_jobs;
        out.cold_no_index_post_restart_gucs_off = gucs_off;
        out.cold_no_index_post_restart_mpf = mpf;
        out.cold_no_index_post_restart_mps = mps;
    } else {
        out.cold_no_index_measured_async_jobs = async_jobs;
        out.cold_no_index_measured_gucs_off = gucs_off;
        out.cold_no_index_measured_mpf = mpf;
        out.cold_no_index_measured_mps = mps;
    }

    const auto exact_absence = [](const ColdNoIndexPolicyAbsenceEvidence& evidence) {
        return evidence.target_rows == 1
            && evidence.target_cardinality_metadata_only == 1
            && evidence.source_parent_rows_read == 0
            && evidence.source_fragment_rows_read == 0
            && evidence.source_decoded_bp == 0
            && evidence.physical_index_relations == 0
            && evidence.persisted_index_rows == 0
            && evidence.generation_objects == 0;
    };
    if (!exact_absence(mpf) || !exact_absence(mps)
        || target_rows != mps.target_rows
        || mpf_rows != 0 || mps_rows != 0
        || async_jobs != 0 || gucs_off != 1) {
        std::fprintf(
            stderr,
            "[bench_e2e] COLD-NO-INDEX-ABSENCE-FAIL phase=%s "
            "target_rows=%lld mpf_rows=%lld mps_rows=%lld "
            "async_jobs=%lld gucs_off=%lld "
            "mpf_meta=%lld mpf_source=%lld/%lld/%lld mpf_objects=%lld/%lld/%lld "
            "mps_meta=%lld mps_source=%lld/%lld/%lld mps_objects=%lld/%lld/%lld\n",
            phase == ColdNoIndexAuditPhase::pre_restart ? "pre_restart"
                : phase == ColdNoIndexAuditPhase::post_restart
                    ? "post_restart" : "measured",
            static_cast<long long>(target_rows),
            static_cast<long long>(mpf_rows),
            static_cast<long long>(mps_rows),
            static_cast<long long>(async_jobs),
            static_cast<long long>(gucs_off),
            static_cast<long long>(mpf.target_cardinality_metadata_only),
            static_cast<long long>(mpf.source_parent_rows_read),
            static_cast<long long>(mpf.source_fragment_rows_read),
            static_cast<long long>(mpf.source_decoded_bp),
            static_cast<long long>(mpf.physical_index_relations),
            static_cast<long long>(mpf.persisted_index_rows),
            static_cast<long long>(mpf.generation_objects),
            static_cast<long long>(mps.target_cardinality_metadata_only),
            static_cast<long long>(mps.source_parent_rows_read),
            static_cast<long long>(mps.source_fragment_rows_read),
            static_cast<long long>(mps.source_decoded_bp),
            static_cast<long long>(mps.physical_index_relations),
            static_cast<long long>(mps.persisted_index_rows),
            static_cast<long long>(mps.generation_objects));
        std::fflush(stderr);
        std::abort();
    }
}

struct ColdNoIndexLifecycleGateInput {
    std::int64_t restart_performed = 0;
    std::int64_t restart_identity_changed = 0;
    std::int64_t identity_fields_present = 0;
    std::int64_t alignment_uses_post_restart_backend = 0;
    std::int64_t pre_target_rows = 0;
    std::int64_t post_target_rows = 0;
    std::int64_t pre_mpf_rows = 0;
    std::int64_t pre_mps_rows = 0;
    std::int64_t post_mpf_rows = 0;
    std::int64_t post_mps_rows = 0;
    std::int64_t measured_mpf_rows = 0;
    std::int64_t measured_mps_rows = 0;
    std::int64_t pre_async_jobs = 0;
    std::int64_t post_async_jobs = 0;
    std::int64_t measured_async_jobs = 0;
    std::int64_t pre_gucs_off = 0;
    std::int64_t post_gucs_off = 0;
    std::int64_t measured_gucs_off = 0;
    std::int64_t metadata_only_physical_absence_proven = 0;
};

[[nodiscard]] constexpr bool cold_no_index_lifecycle_gate(
        const ColdNoIndexLifecycleGateInput& evidence) noexcept {
    return evidence.restart_performed == 1
        && evidence.restart_identity_changed == 1
        && evidence.identity_fields_present == 1
        && evidence.alignment_uses_post_restart_backend == 1
        && evidence.pre_target_rows == 1
        && evidence.post_target_rows == 1
        && evidence.pre_mpf_rows == 0
        && evidence.pre_mps_rows == 0
        && evidence.post_mpf_rows == 0
        && evidence.post_mps_rows == 0
        && evidence.measured_mpf_rows == 0
        && evidence.measured_mps_rows == 0
        && evidence.pre_async_jobs == 0
        && evidence.post_async_jobs == 0
        && evidence.measured_async_jobs == 0
        && evidence.pre_gucs_off == 1
        && evidence.post_gucs_off == 1
        && evidence.measured_gucs_off == 1
        && evidence.metadata_only_physical_absence_proven == 1;
}

constexpr ColdNoIndexLifecycleGateInput kColdNoIndexValidGateFixture{
    .restart_performed = 1,
    .restart_identity_changed = 1,
    .identity_fields_present = 1,
    .alignment_uses_post_restart_backend = 1,
    .pre_target_rows = 1,
    .post_target_rows = 1,
    .pre_gucs_off = 1,
    .post_gucs_off = 1,
    .measured_gucs_off = 1,
    .metadata_only_physical_absence_proven = 1,
};
static_assert(cold_no_index_lifecycle_gate(kColdNoIndexValidGateFixture));
static_assert(!cold_no_index_lifecycle_gate([] {
    auto evidence = kColdNoIndexValidGateFixture;
    evidence.restart_performed = 0;
    return evidence;
}()));
static_assert(!cold_no_index_lifecycle_gate([] {
    auto evidence = kColdNoIndexValidGateFixture;
    evidence.restart_identity_changed = 0;
    return evidence;
}()));
static_assert(!cold_no_index_lifecycle_gate([] {
    auto evidence = kColdNoIndexValidGateFixture;
    evidence.post_mpf_rows = 1;
    return evidence;
}()));
static_assert(!cold_no_index_lifecycle_gate([] {
    auto evidence = kColdNoIndexValidGateFixture;
    evidence.pre_mps_rows = 1;
    return evidence;
}()));
static_assert(!cold_no_index_lifecycle_gate([] {
    auto evidence = kColdNoIndexValidGateFixture;
    evidence.post_async_jobs = 1;
    return evidence;
}()));
static_assert(!cold_no_index_lifecycle_gate([] {
    auto evidence = kColdNoIndexValidGateFixture;
    evidence.post_gucs_off = 0;
    return evidence;
}()));
static_assert(!cold_no_index_lifecycle_gate([] {
    auto evidence = kColdNoIndexValidGateFixture;
    evidence.alignment_uses_post_restart_backend = 0;
    return evidence;
}()));
static_assert(!cold_no_index_lifecycle_gate([] {
    auto evidence = kColdNoIndexValidGateFixture;
    evidence.metadata_only_physical_absence_proven = 0;
    return evidence;
}()));

void require_cold_no_index_lifecycle(E2eOutput& out) {
    const bool identity_fields_present =
        !out.cold_no_index_pre_restart_postmaster_start_time.empty()
        && !out.cold_no_index_post_restart_postmaster_start_time.empty()
        && out.cold_no_index_pre_restart_postmaster_start_time
            != out.cold_no_index_post_restart_postmaster_start_time
        && out.cold_no_index_pre_restart_backend_pid > 0
        && out.cold_no_index_post_restart_backend_pid > 0
        && out.cold_no_index_pre_restart_backend_pid
            != out.cold_no_index_post_restart_backend_pid
        && out.cold_no_index_pre_restart_database_oid > 0
        && out.cold_no_index_pre_restart_database_oid
            == out.cold_no_index_post_restart_database_oid;
    const bool alignment_uses_post_restart_backend =
        out.alignment_backend_pid
            == out.cold_no_index_post_restart_backend_pid
        && out.alignment_database_oid
            == out.cold_no_index_post_restart_database_oid
        && out.timed_postmaster_start_time
            == out.cold_no_index_post_restart_postmaster_start_time;
    const auto exact_absence = [](const ColdNoIndexPolicyAbsenceEvidence& evidence) {
        return evidence.target_rows == 1
            && evidence.target_cardinality_metadata_only == 1
            && evidence.source_parent_rows_read == 0
            && evidence.source_fragment_rows_read == 0
            && evidence.source_decoded_bp == 0
            && evidence.physical_index_relations == 0
            && evidence.persisted_index_rows == 0
            && evidence.generation_objects == 0;
    };
    const bool metadata_only_physical_absence_proven =
        exact_absence(out.cold_no_index_pre_restart_mpf)
        && exact_absence(out.cold_no_index_pre_restart_mps)
        && exact_absence(out.cold_no_index_post_restart_mpf)
        && exact_absence(out.cold_no_index_post_restart_mps)
        && exact_absence(out.cold_no_index_measured_mpf)
        && exact_absence(out.cold_no_index_measured_mps);
    const ColdNoIndexLifecycleGateInput evidence{
        .restart_performed = out.cold_no_index_restart_performed,
        .restart_identity_changed =
            out.cold_no_index_restart_identity_changed,
        .identity_fields_present = identity_fields_present ? 1 : 0,
        .alignment_uses_post_restart_backend =
            alignment_uses_post_restart_backend ? 1 : 0,
        .pre_target_rows = out.cold_no_index_pre_restart_target_rows,
        .post_target_rows = out.cold_no_index_post_restart_target_rows,
        .pre_mpf_rows = out.cold_no_index_pre_restart_mpf_rows,
        .pre_mps_rows = out.cold_no_index_pre_restart_mps_rows,
        .post_mpf_rows = out.cold_no_index_post_restart_mpf_rows,
        .post_mps_rows = out.cold_no_index_post_restart_mps_rows,
        .measured_mpf_rows =
            out.cold_no_index_measured_mpf.persisted_index_rows,
        .measured_mps_rows =
            out.cold_no_index_measured_mps.persisted_index_rows,
        .pre_async_jobs = out.cold_no_index_pre_restart_async_jobs,
        .post_async_jobs = out.cold_no_index_post_restart_async_jobs,
        .measured_async_jobs = out.cold_no_index_measured_async_jobs,
        .pre_gucs_off = out.cold_no_index_pre_restart_gucs_off,
        .post_gucs_off = out.cold_no_index_post_restart_gucs_off,
        .measured_gucs_off = out.cold_no_index_measured_gucs_off,
        .metadata_only_physical_absence_proven =
            metadata_only_physical_absence_proven ? 1 : 0,
    };
    out.cold_no_index_lifecycle_accepted =
        cold_no_index_lifecycle_gate(evidence) ? 1 : 0;
    if (out.cold_no_index_lifecycle_accepted == 1) {
        return;
    }
    std::fprintf(
        stderr,
        "[bench_e2e] COLD-NO-INDEX-LIFECYCLE-FAIL restart=%lld "
        "identity_changed=%lld identity_present=%d alignment_fresh=%d "
        "target=%lld/%lld mpf=%lld/%lld/%lld mps=%lld/%lld/%lld "
        "async=%lld/%lld/%lld gucs=%lld/%lld/%lld physical_absence=%d\n",
        static_cast<long long>(out.cold_no_index_restart_performed),
        static_cast<long long>(out.cold_no_index_restart_identity_changed),
        identity_fields_present ? 1 : 0,
        alignment_uses_post_restart_backend ? 1 : 0,
        static_cast<long long>(out.cold_no_index_pre_restart_target_rows),
        static_cast<long long>(out.cold_no_index_post_restart_target_rows),
        static_cast<long long>(out.cold_no_index_pre_restart_mpf_rows),
        static_cast<long long>(out.cold_no_index_post_restart_mpf_rows),
        static_cast<long long>(
            out.cold_no_index_measured_mpf.persisted_index_rows),
        static_cast<long long>(out.cold_no_index_pre_restart_mps_rows),
        static_cast<long long>(out.cold_no_index_post_restart_mps_rows),
        static_cast<long long>(
            out.cold_no_index_measured_mps.persisted_index_rows),
        static_cast<long long>(out.cold_no_index_pre_restart_async_jobs),
        static_cast<long long>(out.cold_no_index_post_restart_async_jobs),
        static_cast<long long>(out.cold_no_index_measured_async_jobs),
        static_cast<long long>(out.cold_no_index_pre_restart_gucs_off),
        static_cast<long long>(out.cold_no_index_post_restart_gucs_off),
        static_cast<long long>(out.cold_no_index_measured_gucs_off),
        metadata_only_physical_absence_proven ? 1 : 0);
    std::fflush(stderr);
    std::abort();
}

void capture_index_audit(
        const it::connection_test_context& c,
        const std::string& table,
        std::string_view minimizer_index,
        E2eOutput& out) {
    out.audit_selected_minimizer_is_sequence =
        minimizer_index == kMinimizersPerSequenceIndex ? 1 : 0;
    out.audit_mpf_rows =
        index_row_count(c, table, kMinimizersPerFragmentIndex);
    out.audit_mps_rows =
        index_row_count(c, table, kMinimizersPerSequenceIndex);

    out.audit_mpf_update_on_insert = -1;
    out.audit_mps_update_on_insert = -1;
    out.audit_mpf_active_read = -1;
    out.audit_mps_active_read = -1;
    const auto policy_rows = it::query_rows(c, fmt::format(
        "SELECT index_name, update_on_insert::int, is_active_read::int "
        "FROM bioseq_internal.index_policy "
        "WHERE user_relid='{}'::regclass "
        "AND main_relfilenode=(SELECT relfilenode FROM pg_class WHERE oid='{}'::regclass) "
        "AND column_attno=(SELECT attnum FROM pg_attribute "
            "WHERE attrelid='{}'::regclass AND attname='seq') "
        "ORDER BY index_name;",
        table,
        table,
        table));
    for (const auto& row : policy_rows) {
        if (row.size() < 3 || !row[0].has_value()) {
            continue;
        }
        const std::string_view name = *row[0];
        const std::int64_t uoi = row[1].has_value() ? parse_i64(*row[1]) : -1;
        const std::int64_t active = row[2].has_value() ? parse_i64(*row[2]) : -1;
        if (name == kMinimizersPerFragmentIndex) {
            out.audit_mpf_update_on_insert = uoi;
            out.audit_mpf_active_read = active;
        } else if (name == kMinimizersPerSequenceIndex) {
            out.audit_mps_update_on_insert = uoi;
            out.audit_mps_active_read = active;
        }
    }

    const auto stats = it::query_rows(c,
        "SELECT async_persist_jobs_waiting FROM bioseq.scheduler_stats();");
    out.audit_async_persist_jobs_waiting = first_i64_or_minus_one(stats);
}

// Validate the policy that setup persisted before the cold restart.  This is
// deliberately called only after the measured statement: validating policy in
// the fresh backend before the timer would warm the exact relation/catalog
// metadata whose cold cost the just_index benchmark is intended to include.
void finalize_post_measure_persisted_policy_audit(
        std::string_view minimizer_index,
        E2eOutput& out) {
    const bool expect_sequence =
        minimizer_index == kMinimizersPerSequenceIndex;
    const bool mpf_selected = !expect_sequence;
    const bool accepted =
        out.audit_selected_minimizer_is_sequence
            == (expect_sequence ? 1 : 0)
        && out.audit_mpf_update_on_insert == (mpf_selected ? 1 : 0)
        && out.audit_mps_update_on_insert == (expect_sequence ? 1 : 0)
        && out.audit_mpf_active_read == (mpf_selected ? 1 : 0)
        && out.audit_mps_active_read == (expect_sequence ? 1 : 0)
        && (mpf_selected ? out.audit_mpf_rows > 0 : out.audit_mpf_rows == 0)
        && (expect_sequence ? out.audit_mps_rows > 0 : out.audit_mps_rows == 0)
        && out.audit_async_persist_jobs_waiting == 0
        && out.audit_index_rows_wait_attempts == 0;
    out.persisted_policy_audit_accepted = accepted ? 1 : 0;
    if (!accepted) {
        bioseq_log<ERR>(
            "[bench_e2e] post-measure persisted-policy audit failed "
            "index={} selected_is_sequence={} mpf_rows={} mps_rows={} "
            "mpf_update={} mps_update={} mpf_active={} mps_active={} "
            "async_waiting={} wait_attempts={}",
            minimizer_index,
            out.audit_selected_minimizer_is_sequence,
            out.audit_mpf_rows,
            out.audit_mps_rows,
            out.audit_mpf_update_on_insert,
            out.audit_mps_update_on_insert,
            out.audit_mpf_active_read,
            out.audit_mps_active_read,
            out.audit_async_persist_jobs_waiting,
            out.audit_index_rows_wait_attempts);
    }
}

void capture_async_persist_waiting(
        const it::connection_test_context& c,
        E2eOutput& out) {
    const auto stats = it::query_rows(c,
        "SELECT async_persist_jobs_waiting FROM bioseq.scheduler_stats();");
    out.audit_async_persist_jobs_waiting = first_i64_or_minus_one(stats);
}

void capture_index_audit_after_async_persist(
        const it::connection_test_context& c,
        const std::string& table,
        std::string_view minimizer_index,
        E2eOutput& out) {
    constexpr int kFragmentMaxAttempts = 180;
    constexpr int kSequenceMaxAttempts = 900;
    constexpr auto kSleep = std::chrono::seconds{1};
    const bool expect_sequence = minimizer_index == kMinimizersPerSequenceIndex;
    const int max_attempts = expect_sequence ? kSequenceMaxAttempts : kFragmentMaxAttempts;
    for (int attempt = 0; attempt <= max_attempts; ++attempt) {
        capture_async_persist_waiting(c, out);
        out.audit_index_rows_wait_attempts = attempt;
        if (out.audit_async_persist_jobs_waiting > 0) {
            std::this_thread::sleep_for(kSleep);
            continue;
        }
        capture_index_audit(c, table, minimizer_index, out);
        const std::int64_t target_rows =
            expect_sequence ? out.audit_mps_rows : out.audit_mpf_rows;
        if (target_rows > 0 || out.audit_async_persist_jobs_waiting == 0) {
            return;
        }
        std::this_thread::sleep_for(kSleep);
    }
}

void attach_resource_metrics(E2eOutput& out,
                             std::uint64_t start_ns,
                             std::uint64_t end_ns,
                             const ResourceSample& resources) {
    out.measured_region_ns = end_ns - start_ns;
    out.peak_postgres_rss_kb = resources.postgres_rss_kb;
    out.peak_minimap2_rss_kb = resources.minimap2_rss_kb;
    out.peak_bench_rss_kb = resources.bench_rss_kb;
    out.min_tmp_available_kb = resources.tmp_available_kb;
    out.peak_swap_used_mb = resources.swap_used_mb;
}

void log_measured_case(std::string_view algo,
                       std::string_view pipeline_mode,
                       int minimap_threads,
                       const E2eInput& in,
                       const E2eOutput& out,
                       std::string_view minimizer_index = "-") {
    bioseq_log<DBG>("[bench_e2e] measured algo={} pipeline_mode={} minimap_threads={} minimizer_index={} size={} measured_ns={} pg_peak_rss_mb={:.2f} minimap2_peak_rss_mb={:.2f} bench_peak_rss_mb={:.2f} tmp_min_free_gib={:.2f} swap_peak_mb={:.2f} found={} offset_bp={}",
            algo,
            pipeline_mode,
            minimap_threads,
            minimizer_index,
            in.size_label,
            static_cast<unsigned long long>(out.measured_region_ns),
            static_cast<double>(out.peak_postgres_rss_kb) / 1024.0,
            static_cast<double>(out.peak_minimap2_rss_kb) / 1024.0,
            static_cast<double>(out.peak_bench_rss_kb) / 1024.0,
            static_cast<double>(out.min_tmp_available_kb) / (1024.0 * 1024.0),
            out.peak_swap_used_mb,
            out.found,
            out.abs_offset_bp);
    bioseq_log<DBG>(
            "[bench_e2e] proof_env workers={}/{} lifecycle=(nodes={},nested={},completed={},partial={},drained={}) persisted=(generation={},source={}) gpu=(uuid={},driver={},cache_policy={},seed_sha256={},seed_size={},manifest={},hit={})",
            out.effective_cpu_workers,
            out.comparison_cpu_workers,
            out.alignment_scan_node_count,
            out.nested_alignment_executor_count,
            out.alignment_completed,
            out.alignment_partial,
            out.alignment_fully_drained,
            out.persisted_generation_id,
            out.persisted_source_identity,
            out.gpu_device_uuid,
            out.gpu_driver,
            out.gpu_pipeline_cache_policy,
            out.gpu_pipeline_cache_seed_sha256,
            out.gpu_pipeline_cache_seed_size,
            out.gpu_pipeline_cache_manifest_checksum,
            out.gpu_pipeline_cache_hit);
}

void cleanup_e2e_input(std::string_view,
                       std::string_view,
                       simdjson::dom::element data,
                       const E2eInput& in) {
    const char* keep_artifacts_env = std::getenv("BIOSEQ_E2E_KEEP_ARTIFACTS");
    const bool keep_artifacts = jbool(
        data, "keep_benchmark_artifacts", false)
        || (keep_artifacts_env != nullptr
            && std::string_view{keep_artifacts_env} == "1");
    if (keep_artifacts) {
        bioseq_log<DBG>("[bench_e2e] keeping SQL tables and FASTA/MMI artifacts "
                "for an explicitly reusable benchmark fixture");
        return;
    }

    if (in.conn != nullptr) {
        it::exec_sqlf(*in.conn, "DROP TABLE IF EXISTS {} CASCADE;",
                      in.just_index_table_simd_per_fragment);
        it::exec_sqlf(*in.conn, "DROP TABLE IF EXISTS {} CASCADE;",
                      in.just_index_table_simd_per_sequence);
        it::exec_sqlf(*in.conn, "DROP TABLE IF EXISTS {} CASCADE;",
                      in.just_index_table_scalar_per_fragment);
        it::exec_sqlf(*in.conn, "DROP TABLE IF EXISTS {} CASCADE;",
                      in.just_index_table_scalar_per_sequence);
        it::exec_sqlf(*in.conn, "DROP TABLE IF EXISTS {} CASCADE;",
                      in.just_index_table_gpu_per_fragment);
        it::exec_sqlf(*in.conn, "DROP TABLE IF EXISTS {} CASCADE;",
                      in.just_index_table_gpu_per_sequence);
        it::exec_sqlf(*in.conn, "DROP TABLE IF EXISTS {} CASCADE;", in.no_index_table);
    }

    for (const auto& path : {in.target_fasta, in.target_fasta_split,
                             in.query_fasta, in.mmi_path}) {
        std::error_code ec;
        std::filesystem::remove(path, ec);
        if (ec) {
            bioseq_log<WRN>("[bench_e2e] cleanup could not remove '{}': {}", path, ec.message());
        }
    }
}

// ---- §10.1 on_estimate ----
template <sequence_alphabet A>
[[nodiscard]] RunConstraints e2e_estimate_typed(simdjson::dom::element data) {
    using traits = compact_sequence_range<bench_config<A>>::traits;
    using storage_unit_t = typename bench_config<A>::storage_unit;
    const auto ref_bp = static_cast<std::size_t>(
        jint(data, "reference_length", kDefaultReferenceLengthBp));
    const auto query_bp = static_cast<std::size_t>(
        jint(data, "fragment_length_mean", 5'000'000));
    const std::size_t packed_units =
        (ref_bp + traits::letters_per_unit - 1) / traits::letters_per_unit;
    const std::size_t packed_bytes = packed_units * sizeof(storage_unit_t);
    const std::size_t unpacked_reference_bytes = ref_bp * sizeof(A);
    const std::size_t query_bytes = query_bp * sizeof(A);
    return {
        .base_memory = packed_bytes + query_bytes,
        .extra_memory = unpacked_reference_bytes + query_bytes
    };
}

[[nodiscard]] RunConstraints e2e_estimate(std::string_view, std::string_view,
                                          simdjson::dom::element data) {
    RunConstraints out{};
    dispatch_bench_alphabet(jstr(data, "alphabet", "dna4"),
        [&out, data]<sequence_alphabet A>() {
            out = e2e_estimate_typed<A>(data);
        });
    return out;
}

// ---- §10.2 on_input (all untimed setup) ----
template <sequence_alphabet A>
[[nodiscard]] E2eInput e2e_input_typed(std::string_view /*data_dir*/,
                                       std::string_view test_case_name,
                                       simdjson::dom::element data,
                                       const RunConstraints& constraints) {
    gen::generator_params gp{};
    gp.reference_length       = static_cast<std::size_t>(jint(data, "reference_length", kDefaultReferenceLengthBp));
    gp.fragment_count         = static_cast<std::size_t>(jint(data, "fragment_count", 1));
    gp.fragment_length_mean   = static_cast<std::size_t>(jint(data, "fragment_length_mean", 5000000));
    gp.fragment_length_stddev = jdbl(data, "fragment_length_stddev", 100000.0);
    gp.substitution_rate      = jdbl(data, "substitution_rate", 0.001);
    gp.insertion_rate         = jdbl(data, "insertion_rate", 0.0005);
    gp.deletion_rate          = jdbl(data, "deletion_rate", 0.0005);
    gp.insertion_length_mean  = jdbl(data, "insertion_length_mean", 1.0);
    gp.deletion_length_mean   = jdbl(data, "deletion_length_mean", 1.0);
    gp.reverse_complement_rate= jdbl(data, "reverse_complement_rate", 0.5);
    gp.seed                   = static_cast<std::uint64_t>(jint(data, "seed", 3219876));
    gp.use_vulkan             = jbool(data, "use_vulkan_generation", false);

    E2eInput in;
    in.alphabet    = std::string{bench_alphabet_name<A>()};
    in.seq_type    = bench_seq_type<A>();
    in.size_label  = jstr(data, "size_label", "x");
    in.kmer_size   = static_cast<int>(jint(data, "kmer_size", kDefaultKmerSize));
    in.window_size = static_cast<int>(jint(data, "window_size", kDefaultWindowSize));
    in.cigar_projection = jbool(data, "cigar_projection", false);
    in.designed_indel_cigar = jbool(data, "designed_indel_cigar", false);
    in.designed_many_gap_cigar = jbool(data, "designed_many_gap_cigar", false);
    in.require_cigar_indel = jbool(data, "require_cigar_indel", false);
    in.min_cigar_deletion_ops =
        static_cast<std::uint32_t>(jint(data, "min_cigar_deletion_ops", 0));
    in.min_cigar_insertion_ops =
        static_cast<std::uint32_t>(jint(data, "min_cigar_insertion_ops", 0));
    in.minimap2_split_bp = static_cast<std::size_t>(jint(data, "minimap2_split_bp", kMinimap2SplitBp));
    const std::size_t split_bp = in.minimap2_split_bp;
    using active_traits = typename bench_sequence_range<A>::traits;
    if (split_bp > static_cast<std::size_t>(kMinimap2MaxRecordBp) ||
        split_bp % active_traits::letters_per_unit != 0) {
        const std::size_t lpu =
            static_cast<std::size_t>(active_traits::letters_per_unit);
        // Next value that is >= split_bp and a multiple of letters_per_unit, so the
        // error can suggest a concrete fix. (lpu is always >= 1.)
        const std::size_t suggested =
            (split_bp % lpu == 0) ? split_bp : ((split_bp / lpu) + 1u) * lpu;
        bioseq_log<ERR>("[bench_e2e] minimap2_split_bp={} is invalid for alphabet {} "
             "(letters_per_unit={}, max={})",
             split_bp, in.alphabet, active_traits::letters_per_unit,
             kMinimap2MaxRecordBp);
        // CRITICAL: bioseq_log is ASYNC (fmtlog) and routes to a file/PG sink that
        // is easily clobbered, so the ERR above is LOST when std::abort() fires
        // before the async flush — which makes a bad config look like a silent
        // SIGABRT (exit 134) with zero output. Mirror the position-tolerance abort
        // above: write the misconfig to stderr SYNCHRONOUSLY with an actionable fix
        // so a JSON misconfiguration always fails loudly. GOTCHA for config authors:
        // minimap2_split_bp MUST be divisible by the alphabet's letters_per_unit
        // (dna4=32, dna5=21, ...). The "reference_length + 352" idiom only stays
        // divisible by coincidence (true for 32 always, for 21 only sometimes).
        const std::string msg = fmt::format(
            "[bench_e2e] FATAL config error: minimap2_split_bp={} is invalid for "
            "alphabet '{}': it must be <= {} AND divisible by letters_per_unit={}. "
            "Set minimap2_split_bp to a multiple of {} (e.g. {}).\n",
            split_bp, in.alphabet,
            static_cast<long long>(kMinimap2MaxRecordBp), lpu, lpu, suggested);
        std::fputs(msg.c_str(), stderr);
        std::fflush(stderr);
        std::abort();
    }

    const std::string tmp = "/tmp";
    in.target_fasta        = fmt::format("{}/e2e_{}_target.fasta",        tmp, in.size_label);
    in.target_fasta_split  = fmt::format("{}/e2e_{}_target.split.fasta",  tmp, in.size_label);
    in.query_fasta         = fmt::format("{}/e2e_{}_query.fasta",         tmp, in.size_label);
    in.mmi_path            = fmt::format("{}/e2e_{}_target.mmi",          tmp, in.size_label);
    in.just_index_table_simd_per_fragment =
        fmt::format("e2e_{}_just_simd_mpf", in.size_label);
    in.just_index_table_simd_per_sequence =
        fmt::format("e2e_{}_just_simd_mps", in.size_label);
    in.just_index_table_scalar_per_fragment =
        fmt::format("e2e_{}_just_scalar_mpf", in.size_label);
    in.just_index_table_scalar_per_sequence =
        fmt::format("e2e_{}_just_scalar_mps", in.size_label);
    in.just_index_table_gpu_per_fragment =
        fmt::format("e2e_{}_just_gpu_mpf", in.size_label);
    in.just_index_table_gpu_per_sequence =
        fmt::format("e2e_{}_just_gpu_mps", in.size_label);
    in.just_index_table_per_fragment = in.just_index_table_simd_per_fragment;
    in.just_index_table_per_sequence = in.just_index_table_simd_per_sequence;
    in.just_index_table = in.just_index_table_per_sequence;
    in.no_index_table = fmt::format("e2e_{}_noidx", in.size_label);

    /* Parse reuse before any multi-gigabyte artifact write.  Query truth is
     * still regenerated deterministically in memory, but a retained fixture
     * must not rewrite roughly 4 GiB of target FASTA and evict the very cold
     * index pages that the next reader-only iteration intends to measure. */
    const char* skip_index_setup_env =
        std::getenv("BIOSEQ_E2E_SKIP_INDEX_BASELINE_SETUP");
    const bool skip_index_baseline_setup =
        jbool(data, "skip_index_baseline_setup", false)
        || (skip_index_setup_env != nullptr
            && std::string_view{skip_index_setup_env} == "1");

    // (1) generate one shared target plus N query fragments.
    auto pairs = gen::alphabet_sequence_generator<A, bench_storage_t>(gp)
                     .generate_sequences_pairs(gp.fragment_count);
    if (pairs.empty()) {
        std::fputs("[bench_e2e] generated zero query fragments\n", stderr);
        std::abort();
    }
    in.queries.clear();
    in.queries.reserve(pairs.size());
    for (std::size_t i = 0; i < pairs.size(); ++i) {
        const bench_pair_t<A>& pair = pairs[i];
        in.queries.push_back(E2eQueryTruth{
            .id = static_cast<std::int32_t>(i),
            .bases = pair.query.to_string(),
            .truth_target_start =
                static_cast<std::int64_t>(pair.truth_target_start),
            .truth_target_end =
                static_cast<std::int64_t>(pair.truth_target_end),
            .truth_is_reverse = pair.is_reverse,
        });
        const std::size_t target_window_start = pair.truth_target_start
            > static_cast<std::size_t>(kPositionToleranceBp)
            ? pair.truth_target_start
                - static_cast<std::size_t>(kPositionToleranceBp)
            : 0u;
        const std::size_t target_window_end = std::min(
            pair.target->bp_size(),
            pair.truth_target_end
                + static_cast<std::size_t>(kPositionToleranceBp));
        in.queries.back().target_window_start = static_cast<std::int64_t>(
            target_window_start);
        in.queries.back().target_window_bases = decode_target_window<A>(
            *pair.target, target_window_start, target_window_end);
        if (in.designed_indel_cigar) {
            if (in.queries.back().truth_is_reverse) {
                std::fputs(
                    "[bench_e2e] designed indel CIGAR case requires "
                    "reverse_complement_rate=0.0\n",
                    stderr);
                std::fflush(stderr);
                std::abort();
            }
            apply_designed_indel_cigar_mutation(in.queries.back(), i);
        }
        if (in.designed_many_gap_cigar) {
            if (in.queries.back().truth_is_reverse) {
                std::fputs(
                    "[bench_e2e] designed many-gap CIGAR case requires "
                    "reverse_complement_rate=0.0\n",
                    stderr);
                std::fflush(stderr);
                std::abort();
            }
            apply_designed_many_gap_cigar_mutation(in.queries.back());
        }
    }
    const bench_pair_t<A>& first = pairs.front();
    in.truth_target_start = static_cast<std::int64_t>(first.truth_target_start);
    in.truth_is_reverse   = first.is_reverse;
    in.target_bp          = static_cast<std::int64_t>(gp.reference_length);

    // (2) Write artifacts only while creating the fixture.  Reuse keeps the
    // existing files byte-for-byte; SQL-only cells do not consume them, while
    // minimap/no-index cells can reuse the retained deterministic copies.
    if (!skip_index_baseline_setup) {
        write_single_record_fasta<A>(in.target_fasta, "ref", *first.target);
        write_split_fasta<A>(
            in.target_fasta_split, "ref", *first.target, split_bp);
        write_multi_query_fasta<A>(in.query_fasta, in.queries);
    }
    in.query_bases = in.queries.front().bases;   // compatibility literal path

    /* An explicit skip is an iteration accelerator for reader-only changes:
     * retain the already-published deterministic target/generation and let
     * the same pre/post-restart READY audits below re-attest it.  Resetting the
     * extension here used to erase the very setup that the flag asked us to
     * reuse, making every 2-Gbp reader iteration rebuild for tens of minutes. */
    // (3) connect once; normally establish a clean slate.  The opt-in reuse
    // path keeps the installed extension and exact benchmark tables intact.
    in.conn = std::make_shared<it::connection_test_context>();
    if (!skip_index_baseline_setup) {
        it::reset_extension(*in.conn, {
            std::string_view{in.just_index_table_simd_per_fragment},
            std::string_view{in.just_index_table_simd_per_sequence},
            std::string_view{in.just_index_table_scalar_per_fragment},
            std::string_view{in.just_index_table_scalar_per_sequence},
            std::string_view{in.just_index_table_gpu_per_fragment},
            std::string_view{in.just_index_table_gpu_per_sequence},
            std::string_view{in.no_index_table} });
    } else if (it::query_scalar_int(
            *in.conn,
            "SELECT count(*) FROM pg_extension WHERE extname='bioseq';")
            != 1) {
        std::fputs(
            "[bench_e2e] skip_index_baseline_setup requires an existing "
            "bioseq extension and prebuilt tables\n",
            stderr);
        std::fflush(stderr);
        std::abort();
    }
    preflight_e2e_resources(*in.conn, in.size_label, in.target_bp);

    // (4) force the fastest bioseq combo for the whole session.
    configure_bioseq_simd_session(*in.conn, in.kmer_size, in.window_size);

    const char* skip_minimap2_setup_env =
        std::getenv("BIOSEQ_E2E_SKIP_MINIMAP2_BASELINE_SETUP");
    const bool skip_minimap2_baseline_setup =
        jbool(data, "skip_minimap2_baseline_setup", false)
        || (skip_minimap2_setup_env != nullptr
            && std::string_view{skip_minimap2_setup_env} == "1");

    const char* just_index_setup_only_env =
        std::getenv("BIOSEQ_E2E_JUST_INDEX_SETUP_ONLY");
    const std::string just_index_setup_only_name =
        just_index_setup_only_env != nullptr
            ? std::string{just_index_setup_only_env}
            : jstr(data, "just_index_setup_only", "");
    if (!just_index_setup_only_name.empty()
        && just_index_setup_only_name != kMinimizersPerFragmentIndex
        && just_index_setup_only_name != kMinimizersPerSequenceIndex) {
        bioseq_log<ERR>("[bench_e2e] unsupported BIOSEQ_E2E_JUST_INDEX_SETUP_ONLY='{}'",
             just_index_setup_only_name);
        std::abort();
    }
    if (constraints.execution_tuples.empty()) {
        std::fputs(
            "[bench_e2e] framework supplied no expanded execution tuples "
            "to input setup\n",
            stderr);
        std::fflush(stderr);
        std::abort();
    }
    const E2eJustIndexSetupRequirements setup_requirements =
        e2e_just_index_setup_requirements(constraints);
    const E2eSetupSelection setup_selection =
        e2e_setup_selection_for(setup_requirements);
    const bool setup_only_mismatch =
        just_index_setup_only_name == kMinimizersPerFragmentIndex
            ? setup_requirements.scalar_per_sequence
                || setup_requirements.simd_per_sequence
                || setup_requirements.gpu_per_sequence
            : just_index_setup_only_name == kMinimizersPerSequenceIndex
                ? setup_requirements.scalar_per_fragment
                    || setup_requirements.simd_per_fragment
                    || setup_requirements.gpu_per_fragment
                : false;
    if (setup_only_mismatch) {
        std::fprintf(
            stderr,
            "[bench_e2e] just_index_setup_only='%s' conflicts with the "
            "expanded execution tuple policies\n",
            just_index_setup_only_name.c_str());
        std::fflush(stderr);
        std::abort();
    }
    const char* just_index_setup_pipeline_env =
        std::getenv("BIOSEQ_E2E_JUST_INDEX_SETUP_PIPELINE_MODE");
    const std::string just_index_setup_pipeline_mode =
        just_index_setup_pipeline_env != nullptr
            ? std::string{just_index_setup_pipeline_env}
            : std::string{"task_parallel"};
    if (just_index_setup_pipeline_mode != "single"
        && just_index_setup_pipeline_mode != "parallel"
        && just_index_setup_pipeline_mode != "task_parallel") {
        bioseq_log<ERR>("[bench_e2e] unsupported BIOSEQ_E2E_JUST_INDEX_SETUP_PIPELINE_MODE='{}'",
             just_index_setup_pipeline_mode);
        std::abort();
    }
    const auto setup_just_index_table =
        [&in, &just_index_setup_pipeline_mode](
                const std::string& table,
                std::string_view index_name,
                JustIndexSetupKind setup_kind) {
            create_e2e_table(*in.conn, table, in.seq_type);
            configure_bioseq_scheduler_session(
                *in.conn, just_index_setup_pipeline_mode);
            if (setup_kind == JustIndexSetupKind::Gpu) {
                // DOC0345 Patch L — Vulkan minimizer + GPU-index GUCs BEFORE the
                // persisted build so the untimed GPU just_index build runs the GPU
                // minimizer extraction.
                configure_bioseq_vulkan_index_session(
                    *in.conn,
                    in.kmer_size,
                    in.window_size);
                it::exec_sql(
                    *in.conn,
                    "DO $$ BEGIN "
                    "PERFORM current_setting('bioseq.aligner.vulkan_enabled'); "
                    "IF current_setting('bioseq.aligner.minimizer_algorithm') "
                    "   <> 'vulkan_minimap_int' THEN "
                    "  RAISE EXCEPTION 'GPU just_index setup did not select Vulkan minimizers'; "
                    "END IF; "
                    "IF current_setting('bioseq.aligner.vulkan_gpu_index_build_enabled') "
                    "   <> 'on' THEN "
                    "  RAISE EXCEPTION 'GPU just_index setup did not enable GPU index build'; "
                    "END IF; "
                    "END $$;");
            } else if (setup_kind == JustIndexSetupKind::Simd) {
                configure_bioseq_simd_session(
                    *in.conn,
                    in.kmer_size,
                    in.window_size);
                it::exec_sql(
                    *in.conn,
                    "DO $$ BEGIN "
                    "IF current_setting('bioseq.aligner.minimizer_algorithm') "
                    "   <> 'two_stack_nthash' THEN "
                    "  RAISE EXCEPTION 'SIMD just_index setup did not select two_stack_nthash'; "
                    "END IF; "
                    "IF current_setting('bioseq.aligner.chaining_algorithm') "
                    "   <> 'simd_mm2' THEN "
                    "  RAISE EXCEPTION 'SIMD just_index setup did not select simd_mm2'; "
                    "END IF; "
                    "IF current_setting('bioseq.aligner.vulkan_enabled') "
                    "   <> 'off' THEN "
                    "  RAISE EXCEPTION 'SIMD just_index setup left Vulkan enabled'; "
                    "END IF; "
                    "END $$;");
            } else {
                configure_bioseq_scalar_session(
                    *in.conn,
                    in.kmer_size,
                    in.window_size);
                it::exec_sql(
                    *in.conn,
                    "DO $$ BEGIN "
                    "IF current_setting('bioseq.aligner.minimizer_algorithm') "
                    "   <> 'scalar' THEN "
                    "  RAISE EXCEPTION 'Scalar just_index setup did not select scalar minimizers'; "
                    "END IF; "
                    "IF current_setting('bioseq.aligner.chaining_algorithm') "
                    "   <> 'scalar_mm2' THEN "
                    "  RAISE EXCEPTION 'Scalar just_index setup did not select scalar_mm2'; "
                    "END IF; "
                    "IF current_setting('bioseq.aligner.base_alignment_algorithm') "
                    "   <> 'ksw2' THEN "
                    "  RAISE EXCEPTION 'Scalar just_index setup did not select KSW2'; "
                    "END IF; "
                    "IF current_setting('bioseq.aligner.vulkan_enabled') "
                    "   <> 'off' THEN "
                    "  RAISE EXCEPTION 'Scalar just_index setup left Vulkan enabled'; "
                    "END IF; "
                    "END $$;");
            }
            configure_bioseq_minimizer_policy(*in.conn, table, index_name);
            /* The production Format-5 MPF publisher currently seals an
             * authenticated MPF5 mmap sidecar from the completely reread
             * PostgreSQL generation.  Its descriptor provider therefore
             * remains postgres_relation even when the timed reader consumes
             * only the nested compact-H32 file image.  The distinct
             * mapped_file provider lifecycle is deliberately fail-closed
             * until its direct-write publication/retirement/reaping contract
             * is complete; selecting it here would reject setup before any
             * benchmark sample exists. */
            it::exec_sql(
                *in.conn,
                "SET bioseq.index_payload_storage_backend = 'postgres_relation';");
            // A just_index sample owns a fully published PostgreSQL generation
            // before timing starts.  Load only the base sequence first, then
            // build the selected minimizer index explicitly: relying on the
            // INSERT hook writes the legacy layout and does not establish the
            // READY-generation identity that the post-restart reader proves.
            it::exec_sql(*in.conn, "SET bioseq.update_index_on_insert = off;");
            it::exec_sql(
                *in.conn,
                "SET bioseq.aligner.grouped32_persisted_generation_enabled = on;");
            it::exec_sql(*in.conn, "SET bioseq.index_async_persist = off;");
            it::exec_sql(*in.conn, "SET bioseq.recompute_async_persist = off;");
            it::exec_sql(*in.conn, "SET bioseq.read_sequence_index = on;");
            it::exec_sqlf(
                *in.conn,
                "SELECT bioseq.load('{}'::regclass, '{}');",
                table,
                in.target_fasta);
            const std::int64_t sequence_rowno =
                capture_only_sequence_rowno(*in.conn, table);
            const std::int64_t indexed_sequences = it::query_scalar_int(
                *in.conn,
                fmt::format(
                    "SELECT bioseq.update_index('{}'::regclass, '{}');",
                    table,
                    index_name));
            if (indexed_sequences <= 0) {
                bioseq_log<ERR>(
                    "[bench_e2e] just_index setup published no generation "
                    "for table={} index={}",
                    table,
                    index_name);
                std::abort();
            }
            if (!e2e_expects_format6(in, index_name)) {
            const std::string audit_sql = fmt::format(
                "SELECT current_setting('bioseq.aligner.minimizer_algorithm'), "
                "current_setting('bioseq.aligner.vulkan_enabled'), "
                "current_setting('bioseq.aligner.vulkan_gpu_index_build_enabled'), "
                "(current_setting('bioseq.aligner.grouped32_persisted_generation_enabled') "
                " = 'on')::int, "
                "(current_setting('bioseq.update_index_on_insert') = 'off')::int, "
                "(current_setting('bioseq.index_async_persist') = 'off')::int, "
                "(current_setting('bioseq.recompute_async_persist') = 'off')::int, "
                "generation_id, sequence_rowno, gpu_abi_version, total_bp_size, "
                "kmer_size, window_size "
                "FROM bioseq.inspect_ready_minimizer_generation("
                "'{}'::regclass, '{}', {})",
                table,
                index_name,
                sequence_rowno);
            const auto audit_rows = it::query_rows(*in.conn, audit_sql);
            const bool audit_shape_valid =
                audit_rows.size() == 1 && audit_rows.front().size() == 13;
            const std::int64_t grouped32_enabled = audit_shape_valid
                ? row_i64_or_minus_one(audit_rows.front(), 3)
                : -1;
            const std::int64_t insert_maintenance_disabled = audit_shape_valid
                ? row_i64_or_minus_one(audit_rows.front(), 4)
                : -1;
            const std::int64_t async_persist_disabled = audit_shape_valid
                ? row_i64_or_minus_one(audit_rows.front(), 5)
                : -1;
            const std::int64_t recompute_persist_disabled = audit_shape_valid
                ? row_i64_or_minus_one(audit_rows.front(), 6)
                : -1;
            const std::int64_t generation_id = audit_shape_valid
                ? row_i64_or_minus_one(audit_rows.front(), 7)
                : -1;
            const std::int64_t observed_sequence_rowno = audit_shape_valid
                ? row_i64_or_minus_one(audit_rows.front(), 8)
                : -1;
            const std::int64_t gpu_abi_version = audit_shape_valid
                ? row_i64_or_minus_one(audit_rows.front(), 9)
                : -1;
            const std::int64_t persisted_bp = audit_shape_valid
                ? row_i64_or_minus_one(audit_rows.front(), 10)
                : -1;
            const std::int64_t persisted_k = audit_shape_valid
                ? row_i64_or_minus_one(audit_rows.front(), 11)
                : -1;
            const std::int64_t persisted_w = audit_shape_valid
                ? row_i64_or_minus_one(audit_rows.front(), 12)
                : -1;
            if (!audit_shape_valid
                || indexed_sequences <= 0
                || grouped32_enabled != 1
                || insert_maintenance_disabled != 1
                || async_persist_disabled != 1
                || recompute_persist_disabled != 1
                || generation_id <= 0
                || observed_sequence_rowno != sequence_rowno
                || gpu_abi_version != 3
                || persisted_bp != in.target_bp
                || persisted_k != in.kmer_size
                || persisted_w != in.window_size) {
                bioseq_log<ERR>(
                    "[bench_e2e] invalid just_index READY-generation setup "
                    "for table={} index={} indexed_sequences={} generation={} "
                    "sequence_rowno={}/{} gpu_abi={} persisted_bp={} "
                    "expected_bp={} k={} w={} "
                    "grouped32={} insert_off={} async_off={} recompute_off={}",
                    table,
                    index_name,
                    indexed_sequences,
                    generation_id,
                    observed_sequence_rowno,
                    sequence_rowno,
                    gpu_abi_version,
                    persisted_bp,
                    in.target_bp,
                    persisted_k,
                    persisted_w,
                    grouped32_enabled,
                    insert_maintenance_disabled,
                    async_persist_disabled,
                    recompute_persist_disabled);
                std::abort();
            }
            }
            const bool inserted =
                in.just_index_sequence_rowno_by_table.emplace(
                    table, sequence_rowno).second;
            if (!inserted) {
                std::fprintf(
                    stderr,
                    "[bench_e2e] duplicate captured source row identity for "
                    "just_index table=%s\n",
                    table.c_str());
                std::fflush(stderr);
                std::abort();
            }
        };

    const auto reuse_just_index_table = [&in](const std::string& table) {
        const std::int64_t sequence_rowno =
            capture_only_sequence_rowno(*in.conn, table);
        const bool inserted = in.just_index_sequence_rowno_by_table.emplace(
            table, sequence_rowno).second;
        if (!inserted) {
            std::fprintf(
                stderr,
                "[bench_e2e] duplicate reused source-row identity for "
                "just_index table=%s\n",
                table.c_str());
            std::fflush(stderr);
            std::abort();
        }
    };

    // (5) just_index tables: each indexed cell gets its own table and policy.
    // This avoids a per-sequence run accidentally succeeding through a residual
    // per-fragment index, and keeps the per-fragment cell valid too.  Reuse
    // captures only the table-AM row key here; the exact generation/source/
    // compatibility audit still runs immediately before and after the cold
    // measured statement, so a stale or mismatched retained setup fails closed.
    if (skip_index_baseline_setup && setup_requirements.any_bioseq()) {
        if (setup_requirements.scalar_per_fragment) {
            reuse_just_index_table(
                in.just_index_table_scalar_per_fragment);
        }
        if (setup_requirements.simd_per_fragment) {
            reuse_just_index_table(
                in.just_index_table_simd_per_fragment);
        }
        if (setup_requirements.gpu_per_fragment) {
            reuse_just_index_table(
                in.just_index_table_gpu_per_fragment);
        }
        if (setup_requirements.scalar_per_sequence) {
            reuse_just_index_table(
                in.just_index_table_scalar_per_sequence);
        }
        if (setup_requirements.simd_per_sequence) {
            reuse_just_index_table(
                in.just_index_table_simd_per_sequence);
        }
        if (setup_requirements.gpu_per_sequence) {
            reuse_just_index_table(
                in.just_index_table_gpu_per_sequence);
        }
    } else if (!skip_index_baseline_setup
            && setup_requirements.any_bioseq()) {
        if (setup_requirements.scalar_per_fragment) {
            setup_just_index_table(
                in.just_index_table_scalar_per_fragment,
                kMinimizersPerFragmentIndex,
                JustIndexSetupKind::Scalar);
        }
        if (setup_requirements.simd_per_fragment) {
            setup_just_index_table(
                in.just_index_table_simd_per_fragment,
                kMinimizersPerFragmentIndex,
                JustIndexSetupKind::Simd);
        }
        if (setup_requirements.gpu_per_fragment) {
            setup_just_index_table(
                in.just_index_table_gpu_per_fragment,
                kMinimizersPerFragmentIndex,
                JustIndexSetupKind::Gpu);
        }
        if (setup_requirements.scalar_per_sequence) {
            setup_just_index_table(
                in.just_index_table_scalar_per_sequence,
                kMinimizersPerSequenceIndex,
                JustIndexSetupKind::Scalar);
        }
        if (setup_requirements.simd_per_sequence) {
            setup_just_index_table(
                in.just_index_table_simd_per_sequence,
                kMinimizersPerSequenceIndex,
                JustIndexSetupKind::Simd);
        }
        if (setup_requirements.gpu_per_sequence) {
            setup_just_index_table(
                in.just_index_table_gpu_per_sequence,
                kMinimizersPerSequenceIndex,
                JustIndexSetupKind::Gpu);
        }
    } else {
        bioseq_log<DBG>("[bench_e2e] skipping bioseq persisted-index setup "
                "(skip_index_baseline_setup={}, scalar_just_index={}, simd_just_index={}, gpu_just_index={})",
                skip_index_baseline_setup,
                setup_selection.scalar_just_index,
                setup_selection.simd_just_index,
                setup_selection.gpu_just_index);
    }

    // (6) no_index table: create empty. Each rep loads base data untimed, proves
    // both minimizer sidecars absent, restarts PostgreSQL, then times only
    // transient index construction + mapping.
    create_e2e_table(*in.conn, in.no_index_table, in.seq_type);

    // (7) minimap2 prebuilt .mmi for the just_index baseline (untimed).
    if (!skip_index_baseline_setup && !skip_minimap2_baseline_setup
        && setup_requirements.minimap2) {
        run_cmd(fmt::format("{} -k{} -w{} -d {} {}", BIOSEQ_E2E_MINIMAP2_BIN,
                            in.kmer_size, in.window_size, in.mmi_path, in.target_fasta_split));
    } else if (skip_minimap2_baseline_setup) {
        bioseq_log<DBG>("[bench_e2e] skipping minimap2 .mmi baseline setup because "
                "BIOSEQ_E2E_SKIP_MINIMAP2_BASELINE_SETUP=1");
    } else if (!setup_requirements.minimap2) {
        bioseq_log<DBG>("[bench_e2e] skipping minimap2 .mmi setup because "
                "minimap2_just_index is not selected");
    }

    bioseq_log<DBG>("[bench_e2e] input '{}' ready (truth_start={}, rev={})",
            test_case_name, in.truth_target_start, in.truth_is_reverse);
    return in;
}

[[nodiscard]] E2eInput e2e_input(std::string_view data_dir,
                                 std::string_view test_case_name,
                                 simdjson::dom::element data,
                                 const RunConstraints& constraints) {
    E2eInput out;
    dispatch_bench_alphabet(jstr(data, "alphabet", "dna4"),
        [&out, data_dir, test_case_name, data, &constraints]<sequence_alphabet A>() {
            out = e2e_input_typed<A>(data_dir, test_case_name, data, constraints);
        });
    return out;
}

// ---- §10.3 on_run (the timed region, dispatched by mode) ----
// Cold-read methodology for the just_index paths. The persisted minimizer index
// is built (untimed) in on_input(); to measure a *cold* assemble-from-storage —
// the index exists on disk but is NOT resident in the backend-local assembled
// index cache nor in shared_buffers — we restart PostgreSQL immediately before
// the timed region. The restart command is environment-specific, so it is taken
// from BIOSEQ_E2E_PG_RESTART_CMD (e.g. on macOS / Postgres.app:
//   arch -arm64 .../pg_ctl -D '<datadir>' restart -m fast -w > /tmp/r.log 2>&1).
// When the env var is unset, abort before timing. Warm just_index timings are
// not comparable to cold restarted reads.
struct PostmasterIdentity {
    std::string start_time;
    std::int64_t backend_pid = 0;
    std::int64_t database_oid = 0;
};

struct ColdRestartProof {
    PostmasterIdentity before{};
    PostmasterIdentity after{};
};

[[nodiscard]] bool e2e_expects_format6(
        const E2eInput& in,
        std::string_view minimizer_index) noexcept {
    return minimizer_index == "minimizers_per_sequence"
        && in.alphabet == "dna4"
        && in.kmer_size == 15
        && in.window_size == 10;
}

[[nodiscard]] PostmasterIdentity capture_postmaster_identity(
        const it::connection_test_context& connection) {
    const auto rows = it::query_rows(
        connection,
        "SELECT pg_postmaster_start_time()::text, pg_backend_pid()::bigint, "
        "(SELECT oid::bigint FROM pg_database "
        " WHERE datname=current_database());");
    if (rows.size() != 1 || rows.front().size() != 3
        || !rows.front()[0].has_value() || rows.front()[0]->empty()) {
        abort_pipeline_proof_capture(
            "postmaster identity has an invalid row shape");
    }
    const auto backend_pid = parse_snapshot_i64(rows.front()[1]);
    const auto database_oid = parse_snapshot_i64(rows.front()[2]);
    if (!backend_pid.has_value() || *backend_pid == 0
        || !database_oid.has_value() || *database_oid == 0) {
        abort_pipeline_proof_capture(
            "postmaster identity is missing or ill-typed");
    }
    return PostmasterIdentity{
        .start_time = *rows.front()[0],
        .backend_pid = *backend_pid,
        .database_oid = *database_oid,
    };
}

[[nodiscard]] nlohmann::json capture_format6_ready_audit(
        const it::connection_test_context& connection,
        const std::string& table,
        const std::string_view minimizer_index,
        const std::int64_t sequence_rowno,
        const E2eInput& in) {
    const it::row_t row = it::query_one(
        connection,
        fmt::format(
            "SELECT bioseq.inspect_ready_minimizer_format6_generation("
            "'{}'::regclass, '{}', {});",
            table,
            minimizer_index,
            sequence_rowno));
    if (row.size() != 1) {
        abort_pipeline_proof_capture(
            "Format-6 READY audit has an invalid scalar shape");
    }
    const nlohmann::json audit = parse_exact_json_cell(
        row.front(), "Format-6 READY audit JSON failed to parse");
    if (!json_has_exact_keys(audit, kFormat6ReadyAuditKeys)
        || required_json_text(audit, "schema")
            != "bioseq.ready_minimizer_format6_generation"
        || required_json_counter(audit, "version") != 1
        || required_json_counter(audit, "format_version") != 6) {
        abort_pipeline_proof_capture(
            "Format-6 READY audit has the wrong exact top-level schema");
    }

    const nlohmann::json& source =
        required_json_object(audit, "source");
    const nlohmann::json& head =
        required_json_object(audit, "head");
    const nlohmann::json& manifest =
        required_json_object(audit, "manifest");
    const nlohmann::json& metadata =
        required_json_object(audit, "metadata");
    const nlohmann::json& compatibility =
        required_json_object(manifest, "compatibility");
    const nlohmann::json& streams =
        required_json_array(audit, "streams");
    if (!json_has_exact_keys(source, kFormat6ReadySourceKeys)
        || !json_has_exact_keys(head, kFormat6ReadyHeadKeys)
        || !json_has_exact_keys(manifest, kFormat6ReadyManifestKeys)
        || !json_has_exact_keys(
            compatibility, kFormat6ReadyCompatibilityKeys)
        || !json_has_exact_keys(metadata, kFormat6ReadyMetadataKeys)
        || streams.size() != 3
        || !head["dense_seal_present"].is_boolean()) {
        abort_pipeline_proof_capture(
            "Format-6 READY audit has the wrong exact nested schema");
    }
    for (const auto& stream : streams) {
        if (!json_has_exact_keys(stream, kFormat6ReadyStreamKeys)) {
            abort_pipeline_proof_capture(
                "Format-6 READY audit stream has the wrong exact schema");
        }
    }

    const auto counter = [](const nlohmann::json& object,
                            const std::string_view key) {
        return required_json_counter(object, key);
    };
    const auto exact_hex = [](const nlohmann::json& object,
                              const std::string_view key,
                              const std::size_t width) {
        return fixed_lower_hex(required_json_text(object, key), width);
    };
    const std::int64_t generation = counter(head, "generation_id");
    const std::int64_t target_length = counter(manifest, "target_length");
    const std::int64_t stored_posting_width =
        counter(compatibility, "stored_posting_width_bytes");
    const bool identity_valid =
        counter(audit, "database_oid") > 0
        && counter(audit, "main_relfilenode") > 0
        && counter(audit, "column_attno") > 0
        && counter(audit, "query_key") == sequence_rowno
        && !required_json_text(audit, "table_name").empty()
        && std::string_view{required_json_text(audit, "index_name")}
            == minimizer_index
        && counter(source, "sequence_rowno") == sequence_rowno
        && counter(source, "locator_version") > 0
        && counter(source, "num_fragments") > 0
        && counter(source, "total_bp_size") == in.target_bp
        && counter(source, "source_epoch") > 0
        && counter(source, "alphabet_code") == 0
        && counter(source, "storage_unit_byte_width") > 0
        && fixed_lower_hex(
            required_json_text(source, "source_identity"), 32)
        && exact_hex(source, "source_content_crc64", 16)
        && generation > 0
        && counter(head, "query_key") == sequence_rowno
        && counter(head, "source_epoch")
            == counter(source, "source_epoch")
        && required_json_text(head, "source_identity")
            == required_json_text(source, "source_identity")
        && required_json_text(head, "source_content_crc64")
            == required_json_text(source, "source_content_crc64")
        && fixed_lower_hex(
            required_json_text(head, "compatibility_digest"), 64)
        && exact_hex(head, "manifest_address_bound_row_crc64", 16)
        && exact_hex(head, "manifest_payload_crc64", 16)
        && exact_hex(head, "head_crc64", 16)
        && counter(manifest, "generation_id") == generation
        && counter(manifest, "source_epoch")
            == counter(source, "source_epoch")
        && required_json_text(manifest, "source_identity")
            == required_json_text(source, "source_identity")
        && required_json_text(manifest, "source_content_crc64")
            == required_json_text(source, "source_content_crc64")
        && required_json_text(manifest, "compatibility_digest")
            == required_json_text(head, "compatibility_digest")
        && exact_hex(manifest, "authenticated_directory_crc64", 16)
        && exact_hex(manifest, "generation_crc64", 16)
        && target_length == in.target_bp
        && counter(manifest, "unique_hash_count") > 0
        && counter(manifest, "total_posting_count") > 0
        && counter(manifest, "total_control_bytes") > 0
        && counter(manifest, "total_posting_bytes") > 0
        && counter(manifest, "directory_entry_count") > 0;
    const bool compatibility_valid =
        counter(compatibility, "alphabet_code") == 0
        && counter(compatibility, "kmer_size") == in.kmer_size
        && counter(compatibility, "window_size") == in.window_size
        && counter(compatibility, "hash_width_bytes") == 4
        && stored_posting_width
            == (target_length <= (std::int64_t{1} << 31) ? 4 : 8)
        && counter(compatibility, "position_encoding_version") == 1
        && counter(compatibility, "hash_semantics_version") == 1
        && counter(compatibility, "count_semantics_version") == 1
        && counter(compatibility, "producer_backend_id") > 0
        && counter(compatibility, "producer_backend_version") > 0
        && counter(compatibility, "controls_per_block")
            == bioseq::indexing::kMinimizerFormat6ControlsPerBlock
        && counter(compatibility, "hash_seed") == 0;
    const bool metadata_valid =
        counter(metadata, "head_rows") == 1
        && counter(metadata, "head_bytes")
            == bioseq::indexing::kMinimizerFormat6HeadBytes
        && counter(metadata, "manifest_rows") == 1
        && counter(metadata, "manifest_bytes")
            == bioseq::indexing::kMinimizerFormat6ManifestBytes;

    const std::array<std::string_view, 3> expected_kinds{
        "directory", "controls", "postings"};
    bool streams_valid = true;
    for (std::size_t index = 0; index < streams.size(); ++index) {
        const auto& stream = streams[index];
        const std::int64_t logical_bytes =
            counter(stream, "logical_bytes");
        const std::int64_t expected_rows =
            logical_bytes
                    / static_cast<std::int64_t>(
                        bioseq::indexing::kMinimizerFormat6RowPayloadBytes)
                + (logical_bytes
                        % static_cast<std::int64_t>(
                            bioseq::indexing::
                                kMinimizerFormat6RowPayloadBytes)
                    != 0
                    ? 1 : 0);
        streams_valid = streams_valid
            && std::string_view{required_json_text(stream, "kind")}
                == expected_kinds[index]
            && logical_bytes > 0
            && counter(stream, "row_count") == expected_rows
            && counter(stream, "exact_catalog_rows")
                == counter(stream, "row_count")
            && counter(stream, "exact_catalog_bytes") == logical_bytes
            && exact_hex(stream, "stream_crc64", 16)
            && required_json_text(stream, "exact_catalog_stream_crc64")
                == required_json_text(stream, "stream_crc64");
    }
    const auto& directory = streams[0];
    const auto& controls = streams[1];
    const auto& postings = streams[2];
    const bool stream_equations =
        static_cast<__int128>(counter(directory, "logical_bytes"))
            == static_cast<__int128>(
                counter(directory, "element_count"))
                * static_cast<__int128>(
                    bioseq::indexing::
                        kMinimizerFormat6DirectoryEntryBytes)
        && counter(directory, "element_count")
            == counter(manifest, "directory_entry_count")
        && required_json_text(directory, "stream_crc64")
            == required_json_text(
                manifest, "authenticated_directory_crc64")
        && counter(controls, "element_count")
            == counter(manifest, "unique_hash_count")
        && counter(controls, "logical_bytes")
            == counter(manifest, "total_control_bytes")
        && counter(postings, "element_count")
            == counter(manifest, "total_posting_count")
        && counter(postings, "logical_bytes")
            == counter(manifest, "total_posting_bytes")
        && static_cast<__int128>(counter(postings, "logical_bytes"))
            == static_cast<__int128>(
                counter(postings, "element_count"))
                * static_cast<__int128>(stored_posting_width);
    if (!identity_valid || !compatibility_valid || !metadata_valid
        || !streams_valid || !stream_equations) {
        abort_pipeline_proof_capture(
            "Format-6 READY audit identity/profile/stream equations disagree");
    }
    return audit;
}

void capture_pre_restart_persisted_identity(
        const it::connection_test_context& connection,
        const std::string& table,
        const std::string_view minimizer_index,
        const std::int64_t sequence_rowno,
        E2eOutput& out) {
    const auto rows = it::query_rows(
        connection,
        fmt::format(
            "SELECT generation_id, source_identity, gpu_abi_version, "
            "sequence_rowno "
            "FROM bioseq.inspect_ready_minimizer_generation("
            "'{}'::regclass, '{}', {})",
            table,
            minimizer_index,
            sequence_rowno));
    if (rows.size() != 1 || rows.front().size() != 4
        || !rows.front()[0].has_value()
        || !rows.front()[1].has_value()
        || !rows.front()[2].has_value()
        || !rows.front()[3].has_value()) {
        abort_pipeline_proof_capture(
            "pre-restart persisted generation identity has an invalid row shape");
    }
    const auto generation = parse_snapshot_i64(rows.front()[0]);
    const auto gpu_abi_version = parse_snapshot_i64(rows.front()[2]);
    const auto observed_sequence_rowno = parse_snapshot_i64(rows.front()[3]);
    if (!generation.has_value() || *generation == 0
        || !fixed_lower_hex(*rows.front()[1], 16)
        || *rows.front()[1] == "0000000000000000"
        || !gpu_abi_version.has_value()
        || *gpu_abi_version != 3
        || !observed_sequence_rowno.has_value()
        || *observed_sequence_rowno != sequence_rowno) {
        abort_pipeline_proof_capture(
            "pre-restart persisted generation is not production Format-5 ABI3");
    }
    out.pre_restart_persisted_generation_id = *generation;
    out.pre_restart_persisted_gpu_abi_version = *gpu_abi_version;
    out.pre_restart_persisted_source_identity = *rows.front()[1];
    out.pre_restart_persisted_format_version = 5;
}

void capture_pre_restart_persisted_audit(
        const it::connection_test_context& connection,
        const std::string& table,
        const std::string_view minimizer_index,
        const std::int64_t sequence_rowno,
        const E2eInput& in,
        E2eOutput& out) {
    if (!e2e_expects_format6(in, minimizer_index)) {
        capture_pre_restart_persisted_identity(
            connection, table, minimizer_index, sequence_rowno, out);
        return;
    }
    out.pre_restart_format6_ready_audit = capture_format6_ready_audit(
        connection, table, minimizer_index, sequence_rowno, in);
    const nlohmann::json& head =
        required_json_object(out.pre_restart_format6_ready_audit, "head");
    const nlohmann::json& source =
        required_json_object(out.pre_restart_format6_ready_audit, "source");
    out.pre_restart_persisted_generation_id =
        required_json_counter(head, "generation_id");
    out.pre_restart_persisted_source_identity =
        required_json_text(source, "source_identity");
    out.pre_restart_persisted_format_version = 6;
}

void capture_post_restart_persisted_audit(
        const it::connection_test_context& connection,
        const std::string& table,
        const std::string_view minimizer_index,
        const std::int64_t sequence_rowno,
        const E2eInput& in,
        E2eOutput& out) {
    if (!e2e_expects_format6(in, minimizer_index)) {
        out.post_restart_persisted_format_version = 5;
        return;
    }
    out.post_restart_format6_ready_audit = capture_format6_ready_audit(
        connection, table, minimizer_index, sequence_rowno, in);
    out.post_restart_persisted_format_version = 6;
    out.format6_restart_audit_accepted =
        out.pre_restart_format6_ready_audit
                == out.post_restart_format6_ready_audit
            ? 1 : 0;
    if (out.format6_restart_audit_accepted != 1) {
        abort_pipeline_proof_capture(
            "Format-6 READY identity/content/profile changed across restart");
    }
}

// Return the exact before/after identity after a successful restart.  Both
// just_index and no_index use this common mechanism but retain disjoint output
// fields and acceptance gates.
[[nodiscard]] ColdRestartProof restart_postgres_for_cold_measure(
        const E2eInput& in,
        const std::string_view lifecycle_name,
        const std::string_view cold_work) {
    const PostmasterIdentity before = capture_postmaster_identity(*in.conn);
    const char* cmd = std::getenv("BIOSEQ_E2E_PG_RESTART_CMD");
    if (cmd == nullptr || *cmd == '\0') {
        bioseq_log<ERR>(
            "[bench_e2e] BIOSEQ_E2E_PG_RESTART_CMD is unset; refusing "
            "to run {} because the configured benchmark requires a cold "
            "post-restart {}. Set BIOSEQ_E2E_PG_RESTART_CMD to a command that "
            "restarts PostgreSQL before every measurement.",
            lifecycle_name,
            cold_work);
        std::fprintf(
            stderr,
            "[bench_e2e] COLD-RESTART-MISSING lifecycle=%.*s work=%.*s: set "
            "BIOSEQ_E2E_PG_RESTART_CMD to a PostgreSQL restart command; "
            "warm measurements are not allowed\n",
            static_cast<int>(lifecycle_name.size()), lifecycle_name.data(),
            static_cast<int>(cold_work.size()), cold_work.data());
        std::fflush(stderr);
        std::abort();
    }
    bioseq_log<INF>(
        "[bench_e2e] restarting PostgreSQL before {} measure "
        "(fresh postmaster/backend for cold {}): {}",
        lifecycle_name,
        cold_work,
        cmd);
    const int rc = std::system(cmd);
    if (rc != 0) {
        bioseq_log<ERR>("[bench_e2e] PostgreSQL restart command failed (rc={}): {} "
            "— the cold {} measurement would be invalid; aborting",
            rc, cmd, lifecycle_name);
        std::abort();
    }
    // The restart terminated the shared session's backend; rebuild it. pg_ctl -w
    // returns only once the server accepts connections, but retry briefly to
    // absorb any residual race / supervisor bgworker startup.
    constexpr int kMaxReconnectAttempts = 40;
    for (int attempt = 1; ; ++attempt) {
        try {
            *in.conn = it::connection_test_context{};
            break;
        } catch (const std::exception& e) {
            if (attempt >= kMaxReconnectAttempts) {
                bioseq_log<ERR>("[bench_e2e] failed to reconnect after PostgreSQL "
                    "restart ({} attempts): {} — aborting", attempt, e.what());
                std::abort();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    }
    // The branch-specific path reapplies its full scheduler/index/backend GUC
    // set; keep the fresh session on the non-Vulkan default until then.
    configure_bioseq_simd_session(*in.conn, in.kmer_size, in.window_size);
    const PostmasterIdentity after = capture_postmaster_identity(*in.conn);
    if (before.start_time == after.start_time
        || before.backend_pid == after.backend_pid
        || before.database_oid != after.database_oid) {
        bioseq_log<ERR>(
            "[bench_e2e] restart command returned without proving a new "
            "postmaster/backend identity (before_start={}, after_start={}, "
            "before_pid={}, after_pid={}, before_db={}, after_db={})",
            before.start_time,
            after.start_time,
            before.backend_pid,
            after.backend_pid,
            before.database_oid,
            after.database_oid);
        std::abort();
    }
    bioseq_log<INF>("[bench_e2e] PostgreSQL restarted + session reconnected; the "
        "{} measurement will execute {} from a fresh backend "
        "(postmaster {} -> {}, backend {} -> {})",
        lifecycle_name,
        cold_work,
        before.start_time,
        after.start_time,
        before.backend_pid,
        after.backend_pid);
    return ColdRestartProof{.before = before, .after = after};
}

// Returns true after a restart has completed and its identity change has been
// proved. A successful restart command alone is not cold-run evidence.
bool restart_postgres_for_cold_just_index(
        const E2eInput& in,
        E2eOutput& out) {
    const ColdRestartProof proof = restart_postgres_for_cold_measure(
        in, "just_index", "persisted-index read plus mapping");
    out.pre_restart_postmaster_start_time = proof.before.start_time;
    out.pre_restart_backend_pid = proof.before.backend_pid;
    out.pre_restart_database_oid = proof.before.database_oid;
    out.post_restart_postmaster_start_time = proof.after.start_time;
    out.post_restart_backend_pid = proof.after.backend_pid;
    out.post_restart_database_oid = proof.after.database_oid;
    out.cold_restart_identity_changed = 1;
    return true;
}

bool restart_postgres_for_cold_no_index(
        const E2eInput& in,
        E2eOutput& out) {
    const ColdRestartProof proof = restart_postgres_for_cold_measure(
        in, "no_index", "transient exact target-index construction plus mapping");
    out.cold_no_index_pre_restart_postmaster_start_time =
        proof.before.start_time;
    out.cold_no_index_pre_restart_backend_pid = proof.before.backend_pid;
    out.cold_no_index_pre_restart_database_oid = proof.before.database_oid;
    out.cold_no_index_post_restart_postmaster_start_time = proof.after.start_time;
    out.cold_no_index_post_restart_backend_pid = proof.after.backend_pid;
    out.cold_no_index_post_restart_database_oid = proof.after.database_oid;
    out.cold_no_index_restart_identity_changed = 1;
    return true;
}

void configure_no_index_storage_session(
        const it::connection_test_context& connection) {
    it::exec_sql(connection, "SET bioseq.update_index_on_insert = off;");
    it::exec_sql(connection, "SET bioseq.read_sequence_index = off;");
    it::exec_sql(connection, "SET bioseq.index_async_persist = off;");
    it::exec_sql(connection, "SET bioseq.recompute_async_persist = off;");
}

void prepare_and_restart_cold_no_index_base(
        const E2eInput& in,
        E2eOutput& out,
        const std::string_view minimizer_index,
        const std::string_view pipeline_mode) {
    // Base PostgreSQL data is setup, not target-index construction.  Load it
    // with both minimizer policies disabled, prove no legacy or revision-3 row
    // exists, then restart so neither shared buffers nor backend-local
    // transient state populated by the load can enter the timed statement.
    configure_bioseq_simd_session(*in.conn, in.kmer_size, in.window_size);
    configure_bioseq_scheduler_session(*in.conn, pipeline_mode);
    it::exec_sqlf(*in.conn, "TRUNCATE {};", in.no_index_table);
    configure_bioseq_minimizer_policy(
        *in.conn, in.no_index_table, minimizer_index);
    configure_no_index_storage_session(*in.conn);
    it::exec_sqlf(
        *in.conn,
        "SELECT bioseq.load('{}'::regclass, '{}')",
        in.no_index_table,
        in.target_fasta);
    capture_cold_no_index_absence(
        *in.conn, in, out, ColdNoIndexAuditPhase::pre_restart);
    out.cold_no_index_restart_performed =
        restart_postgres_for_cold_no_index(in, out) ? 1 : 0;
}

void load_e2e_query_table(const it::connection_test_context& c,
                          const E2eInput& in)
{
    it::exec_sql(c, fmt::format(
        "CREATE TEMP TABLE e2e_queries(id int PRIMARY KEY, seq {});",
        in.seq_type));
    for (const E2eQueryTruth& query : in.queries) {
        it::exec_sql(c, fmt::format(
            "INSERT INTO e2e_queries VALUES ({}, '{}'::{});",
            query.id, query.bases, in.seq_type));
    }
}

[[nodiscard]] std::string batched_coord_sql(
        const std::string& target_table,
        std::string_view projection,
        const int index_read_requirement)
{
    return fmt::format(
        "SELECT q.id, t.header, {} "
        "FROM {} t JOIN e2e_queries q ON mmsearch(t.seq, q.seq, "
        "aligner_options(index_read_requirement => {})) "
        "ORDER BY q.id, t.header",
        projection, target_table, index_read_requirement);
}

[[nodiscard]] std::string batched_transformed_coord_sql(
        const std::string& target_table,
        std::string_view projection,
        const int index_read_requirement,
        const std::int64_t target_coordinate_origin)
{
    const std::int64_t concat_join = target_coordinate_origin + 16;
    return fmt::format(
        "SELECT q.id, t.header, {} "
        "FROM {} t JOIN e2e_queries q ON mmsearch("
        "t.seq[{}:{}] || t.seq[{}:], "
        "q.seq[0:], aligner_options(index_read_requirement => {})) "
        "ORDER BY q.id, t.header",
        projection, target_table, target_coordinate_origin, concat_join,
        concat_join, index_read_requirement);
}

[[nodiscard]] std::int64_t doc0377_transformed_target_origin(
        const E2eInput& in) {
    if (in.queries.empty()) return 0;
    std::int64_t minimum_truth = in.queries.front().truth_target_start;
    for (const E2eQueryTruth& query : in.queries) {
        minimum_truth = std::min(minimum_truth, query.truth_target_start);
    }
    if (minimum_truth <= 0) {
        std::fputs(
            "[bench_e2e] DOC0377 nonzero target slice requires every "
            "generated truth_target_start to be positive\n",
            stderr);
        std::fflush(stderr);
        std::abort();
    }
    return std::min<std::int64_t>(4096, minimum_truth);
}

[[nodiscard]] std::string e2e_projection_sql(const E2eInput& in) {
    if (!in.cigar_projection) {
        return "bioseq.target_start(header) AS tstart, "
               "bioseq.is_reverse(header) AS rev";
    }
    return "bioseq.target_start(header) AS tstart, "
           "bioseq.target_end(header) AS tend, "
           "bioseq.query_start(header) AS qstart, "
           "bioseq.query_end(header) AS qend, "
           "bioseq.is_reverse(header) AS rev, "
           "bioseq.has_cigar(header) AS has_cigar, "
           "bioseq.cigar(header) AS cigar";
}

[[nodiscard]] E2eOutput e2e_run(std::string_view algo, const ParamMap& params,
                                const E2eInput& in) {
    E2eOutput out;
    const std::string pipeline_mode = param_string(params, "pipeline_mode", "single");
    const int fair_workers = fair_scheduler_worker_count(*in.conn);
    const int configured_minimap_threads = param_int(params, "threads", 0);
    if (configured_minimap_threads > 0
        && configured_minimap_threads != fair_workers) {
        std::fprintf(
            stderr,
            "[bench_e2e] WORKER-PARITY-FAIL configured_minimap=%d "
            "bioseq_effective=%d\n",
            configured_minimap_threads,
            fair_workers);
        std::fflush(stderr);
        std::abort();
    }
    /* Zero means exact parity with the live bioseq scheduler.  Bioseq proof
     * later independently attests the same effective worker count. */
    const int minimap_threads = fair_workers;
    out.comparison_cpu_workers = fair_workers;
    const std::string minimizer_index = param_minimizer_index(params);
    const std::string qlit = fmt::format("'{}'::{}", in.query_bases, in.seq_type);
    const std::string proj = e2e_projection_sql(in);

    if (algo == "bioseq_ksw2_scalar_dp_scalar_mm_just_index") {
        const std::string& table = just_index_table_for(
            in, minimizer_index, JustIndexSetupKind::Scalar);
        configure_bioseq_scalar_session(
            *in.conn, in.kmer_size, in.window_size);
        capture_pre_restart_persisted_audit(
            *in.conn, table, minimizer_index,
            just_index_sequence_rowno_for(in, table), in, out);
        out.just_index_restart_performed =
            restart_postgres_for_cold_just_index(in, out) ? 1 : 0;
        configure_bioseq_scheduler_session(*in.conn, pipeline_mode);
        configure_bioseq_pair_sort_session(*in.conn, params);
        configure_bioseq_scalar_session(
            *in.conn, in.kmer_size, in.window_size);
        it::exec_sql(*in.conn, "SET bioseq.read_sequence_index = on;");
        it::exec_sql(
            *in.conn,
            "SET bioseq.aligner.mpf_format5_lookup_route = "
            "'format5_control_scans';");
        const std::string sql = batched_coord_sql(table, proj, 2);
        const AlignmentSnapshotIdentity prior_alignment =
            capture_prior_alignment_identity(*in.conn);
        ScopedResourceMonitor resources;
        const auto start_ns = static_cast<std::uint64_t>(
            bioseq::instr::clock::now_ns());
        {
            auto _scope = bioseq::instr::region(kMeasuredRegion);
            load_e2e_query_table(*in.conn, in);
            parse_bioseq_batch_rows(
                it::query_rows(*in.conn, sql), in, out);
        }
        const auto end_ns = static_cast<std::uint64_t>(
            bioseq::instr::clock::now_ns());
        attach_resource_metrics(out, start_ns, end_ns, resources.stop());
        capture_alignment_statement_stats(
            *in.conn, prior_alignment, out);
        capture_post_restart_persisted_audit(
            *in.conn, table, minimizer_index,
            just_index_sequence_rowno_for(in, table), in, out);
        capture_index_audit(*in.conn, table, minimizer_index, out);
        finalize_post_measure_persisted_policy_audit(minimizer_index, out);
        require_pipeline_execution_proof(
            algo, in, out,
            pipeline_proof_expectation(in, true, fair_workers),
            minimizer_index);
        require_mapped(algo, in, out);
        capture_mps_build_stats(*in.conn, out);
        log_measured_case(
            algo, pipeline_mode, minimap_threads, in, out,
            minimizer_index);

    } else if (algo == "bioseq_just_index"
        || algo == "bioseq_block_simd_dp_two_stack_nthash_just_index") {
        const std::string& table =
            just_index_table_for(in, minimizer_index, JustIndexSetupKind::Simd);
        configure_bioseq_simd_session(
            *in.conn, in.kmer_size, in.window_size);
        capture_pre_restart_persisted_audit(
            *in.conn, table, minimizer_index,
            just_index_sequence_rowno_for(in, table), in, out);
        // Restart PostgreSQL first so the prebuilt index (built untimed in
        // on_input) is assembled cold from storage rather than served warm.
        out.just_index_restart_performed =
            restart_postgres_for_cold_just_index(in, out) ? 1 : 0;
        configure_bioseq_scheduler_session(*in.conn, pipeline_mode);
        configure_bioseq_pair_sort_session(*in.conn, params);
        configure_bioseq_simd_session(*in.conn, in.kmer_size, in.window_size);
        it::exec_sql(*in.conn, "SET bioseq.read_sequence_index = on;");
        /* DOC0383/DOC0384 performance gate: exercise the explicit,
         * fail-closed direct image after the cold restart for MPF.  This is
         * setup, outside the measured statement, and cannot fall back to the
         * three legacy Format-5 control scans. */
        it::exec_sql(
            *in.conn,
            minimizer_index == kMinimizersPerFragmentIndex
                ? "SET bioseq.aligner.mpf_format5_lookup_route = "
                    "'compact_h32_direct';"
                : "SET bioseq.aligner.mpf_format5_lookup_route = "
                    "'format5_control_scans';");
        const std::string sql = batched_coord_sql(table, proj, 2);
        const AlignmentSnapshotIdentity prior_alignment =
            capture_prior_alignment_identity(*in.conn);
        ScopedResourceMonitor resources;
        const auto start_ns = static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
        {
            auto _scope = bioseq::instr::region(kMeasuredRegion);
            load_e2e_query_table(*in.conn, in);
            parse_bioseq_batch_rows(it::query_rows(*in.conn, sql), in, out);
        }
        const auto end_ns = static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
        attach_resource_metrics(out, start_ns, end_ns, resources.stop());
        capture_alignment_statement_stats(
            *in.conn, prior_alignment, out);
        capture_post_restart_persisted_audit(
            *in.conn, table, minimizer_index,
            just_index_sequence_rowno_for(in, table), in, out);
        capture_index_audit(*in.conn, table, minimizer_index, out);
        finalize_post_measure_persisted_policy_audit(minimizer_index, out);
        require_pipeline_execution_proof(
            algo, in, out,
            pipeline_proof_expectation(in, true, fair_workers),
            minimizer_index);
        require_mapped(algo, in, out);
        capture_mps_build_stats(*in.conn, out);
        log_measured_case(
            algo, pipeline_mode, minimap_threads, in, out, minimizer_index);

    } else if (algo == "bioseq_block_vulkan_phase_c_frontend_just_index"
               || algo == "bioseq_block_gpu_dp_gpu_seed_just_index") {
        const std::string& table =
            just_index_table_for(in, minimizer_index, JustIndexSetupKind::Gpu);
        configure_bioseq_vulkan_index_session(
            *in.conn,
            in.kmer_size,
            in.window_size,
            in.cigar_projection);
        capture_pre_restart_persisted_audit(
            *in.conn, table, minimizer_index,
            just_index_sequence_rowno_for(in, table), in, out);
        // DOC0310 Phase C resident two-submit Vulkan frontend applied to the
        // just_index e2e path: index is prebuilt (untimed), only the Vulkan-driven
        // alignment is timed. Same Vulkan settings as bench_v10's
        // bioseq_block_vulkan_phase_c_frontend row.
        // Restart PostgreSQL first so the prebuilt index is assembled cold from
        // storage (GPU seeding/tiled-DP chaining + block aligner over a cold index).
        out.just_index_restart_performed =
            restart_postgres_for_cold_just_index(in, out) ? 1 : 0;
        configure_bioseq_scheduler_session(*in.conn, pipeline_mode);
        configure_bioseq_pair_sort_session(*in.conn, params);
        it::exec_sql(*in.conn, "SET bioseq.read_sequence_index = on;");
        configure_bioseq_vulkan_index_session(
            *in.conn,
            in.kmer_size,
            in.window_size,
            in.cigar_projection);  // DOC0345 Patch L
        const std::string sql = batched_coord_sql(table, proj, 2);
        const AlignmentSnapshotIdentity prior_alignment =
            capture_prior_alignment_identity(*in.conn);
        ScopedResourceMonitor resources;
        const auto start_ns = static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
        {
            auto _scope = bioseq::instr::region(kMeasuredRegion);
            load_e2e_query_table(*in.conn, in);
            parse_bioseq_batch_rows(it::query_rows(*in.conn, sql), in, out);
        }
        const auto end_ns = static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
        attach_resource_metrics(out, start_ns, end_ns, resources.stop());
        capture_alignment_statement_stats(
            *in.conn, prior_alignment, out);
        capture_post_restart_persisted_audit(
            *in.conn, table, minimizer_index,
            just_index_sequence_rowno_for(in, table), in, out);
        capture_index_audit(*in.conn, table, minimizer_index, out);
        finalize_post_measure_persisted_policy_audit(minimizer_index, out);
        require_pipeline_execution_proof(
            algo, in, out,
            pipeline_proof_expectation(in, true, fair_workers),
            minimizer_index);
        require_mapped(algo, in, out);
        capture_mps_build_stats(*in.conn, out);
        log_measured_case(algo, pipeline_mode, minimap_threads, in, out, minimizer_index);

    } else if (algo == "bioseq_ksw2_scalar_dp_scalar_mm_no_index") {
        const bool recompute_async_persist =
            param_string(params, "recompute_async_persist", "off") == "on";
        if (recompute_async_persist) {
            std::fprintf(
                stderr,
                "[bench_e2e] SCALAR-NO-INDEX-ASYNC-PERSIST-FORBIDDEN "
                "algo=%.*s\n",
                static_cast<int>(algo.size()), algo.data());
            std::fflush(stderr);
            std::abort();
        }
        prepare_and_restart_cold_no_index_base(
            in, out, minimizer_index, pipeline_mode);
        configure_bioseq_scalar_session(
            *in.conn, in.kmer_size, in.window_size);
        configure_bioseq_scheduler_session(*in.conn, pipeline_mode);
        configure_bioseq_pair_sort_session(*in.conn, params);
        configure_bioseq_minimizer_policy(
            *in.conn, in.no_index_table, minimizer_index);
        configure_no_index_storage_session(*in.conn);
        capture_cold_no_index_absence(
            *in.conn, in, out, ColdNoIndexAuditPhase::post_restart);
        const std::string align_sql =
            batched_coord_sql(in.no_index_table, proj, 1);
        const AlignmentSnapshotIdentity prior_alignment =
            capture_prior_alignment_identity(*in.conn);
        ScopedResourceMonitor resources;
        const auto start_ns = static_cast<std::uint64_t>(
            bioseq::instr::clock::now_ns());
        {
            auto _scope = bioseq::instr::region(kMeasuredRegion);
            load_e2e_query_table(*in.conn, in);
            parse_bioseq_batch_rows(
                it::query_rows(*in.conn, align_sql), in, out);
        }
        const auto end_ns = static_cast<std::uint64_t>(
            bioseq::instr::clock::now_ns());
        attach_resource_metrics(out, start_ns, end_ns, resources.stop());
        capture_alignment_statement_stats(
            *in.conn, prior_alignment, out);
        capture_mps_build_stats(*in.conn, out);
        capture_index_audit(
            *in.conn, in.no_index_table, minimizer_index, out);
        capture_cold_no_index_absence(
            *in.conn, in, out, ColdNoIndexAuditPhase::measured);
        require_cold_no_index_lifecycle(out);
        require_pipeline_execution_proof(
            algo, in, out,
            pipeline_proof_expectation(in, false, fair_workers),
            minimizer_index);
        require_mapped(algo, in, out);
        log_measured_case(
            algo, pipeline_mode, minimap_threads, in, out,
            minimizer_index);

    } else if (algo == "bioseq_no_index"
               || algo == "bioseq_block_simd_dp_two_stack_nthash_no_index"
               || algo == "bioseq_block_simd_dp_two_stack_nthash_no_index_sliced") {
        const bool recompute_async_persist =
            param_string(params, "recompute_async_persist", "off") == "on";
        if (recompute_async_persist) {
            std::fprintf(
                stderr,
                "[bench_e2e] STANDARD-NO-INDEX-ASYNC-PERSIST-FORBIDDEN "
                "algo=%.*s; use the separately named bioseq_recompute_only "
                "diagnostic case for async-persistence measurements\n",
                static_cast<int>(algo.size()), algo.data());
            std::fflush(stderr);
            std::abort();
        }
        prepare_and_restart_cold_no_index_base(
            in, out, minimizer_index, pipeline_mode);
        configure_bioseq_simd_session(*in.conn, in.kmer_size, in.window_size);
        configure_bioseq_scheduler_session(*in.conn, pipeline_mode);
        configure_bioseq_pair_sort_session(*in.conn, params);
        configure_bioseq_minimizer_policy(
            *in.conn, in.no_index_table, minimizer_index);
        configure_no_index_storage_session(*in.conn);
        capture_cold_no_index_absence(
            *in.conn, in, out, ColdNoIndexAuditPhase::post_restart);
        const bool transformed_recipe =
            algo == "bioseq_block_simd_dp_two_stack_nthash_no_index_sliced";
        const std::int64_t transformed_target_origin = transformed_recipe
            ? doc0377_transformed_target_origin(in) : 0;
        const std::string align_sql = transformed_recipe
            ? batched_transformed_coord_sql(
                in.no_index_table, proj, 1, transformed_target_origin)
            : batched_coord_sql(in.no_index_table, proj, 1);
        const AlignmentSnapshotIdentity prior_alignment =
            capture_prior_alignment_identity(*in.conn);
        ScopedResourceMonitor resources;
        const auto start_ns = static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
        {
            auto _scope = bioseq::instr::region(kMeasuredRegion);
            load_e2e_query_table(*in.conn, in);
            parse_bioseq_batch_rows(
                it::query_rows(*in.conn, align_sql), in, out,
                transformed_target_origin);
        }
        const auto end_ns = static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
        attach_resource_metrics(out, start_ns, end_ns, resources.stop());
        capture_alignment_statement_stats(*in.conn, prior_alignment, out);
        capture_mps_build_stats(*in.conn, out);
        capture_index_audit(
            *in.conn, in.no_index_table, minimizer_index, out);
        capture_cold_no_index_absence(
            *in.conn, in, out, ColdNoIndexAuditPhase::measured);
        require_cold_no_index_lifecycle(out);
        require_pipeline_execution_proof(
            algo, in, out,
            pipeline_proof_expectation(in, false, fair_workers),
            minimizer_index);
        require_mapped(algo, in, out);
        log_measured_case(
            algo, pipeline_mode, minimap_threads, in, out, minimizer_index);

    } else if (algo == "bioseq_block_gpu_dp_gpu_seed_no_index") {
        // The target exists in PostgreSQL before timing, but no minimizer
        // sidecar/generation exists.  Restart after the untimed load, then time
        // only exact GPU transient target-index construction plus mapping.
        const bool recompute_async_persist =
            param_string(params, "recompute_async_persist", "off") == "on";
        if (recompute_async_persist) {
            std::fprintf(
                stderr,
                "[bench_e2e] STANDARD-NO-INDEX-ASYNC-PERSIST-FORBIDDEN "
                "algo=%.*s; use a separately named async-persistence "
                "diagnostic case\n",
                static_cast<int>(algo.size()), algo.data());
            std::fflush(stderr);
            std::abort();
        }
        prepare_and_restart_cold_no_index_base(
            in, out, minimizer_index, pipeline_mode);
        configure_bioseq_vulkan_index_session(
            *in.conn,
            in.kmer_size,
            in.window_size,
            in.cigar_projection);
        configure_bioseq_scheduler_session(*in.conn, pipeline_mode);
        configure_bioseq_pair_sort_session(*in.conn, params);
        configure_bioseq_minimizer_policy(
            *in.conn, in.no_index_table, minimizer_index);
        configure_no_index_storage_session(*in.conn);
        capture_cold_no_index_absence(
            *in.conn, in, out, ColdNoIndexAuditPhase::post_restart);
        const std::string align_sql =
            batched_coord_sql(in.no_index_table, proj, 1);
        const AlignmentSnapshotIdentity prior_alignment =
            capture_prior_alignment_identity(*in.conn);
        ScopedResourceMonitor resources;
        const auto start_ns = static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
        {
            auto _scope = bioseq::instr::region(kMeasuredRegion);
            load_e2e_query_table(*in.conn, in);
            parse_bioseq_batch_rows(
                it::query_rows(*in.conn, align_sql), in, out);
        }
        const auto end_ns = static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
        attach_resource_metrics(out, start_ns, end_ns, resources.stop());
        capture_alignment_statement_stats(*in.conn, prior_alignment, out);
        capture_mps_build_stats(*in.conn, out);
        capture_index_audit(
            *in.conn, in.no_index_table, minimizer_index, out);
        capture_cold_no_index_absence(
            *in.conn, in, out, ColdNoIndexAuditPhase::measured);
        require_cold_no_index_lifecycle(out);
        require_pipeline_execution_proof(
            algo, in, out,
            pipeline_proof_expectation(in, false, fair_workers),
            minimizer_index);
        require_mapped(algo, in, out);
        log_measured_case(
            algo, pipeline_mode, minimap_threads, in, out, minimizer_index);

    } else if (algo == "bioseq_load_only") {
        configure_bioseq_scheduler_session(*in.conn, pipeline_mode);
        configure_bioseq_pair_sort_session(*in.conn, params);
        it::exec_sqlf(*in.conn, "TRUNCATE {};", in.no_index_table);
        configure_bioseq_minimizer_policy(
            *in.conn, in.no_index_table, minimizer_index);
        it::exec_sql(*in.conn, "SET bioseq.update_index_on_insert = off;");
        it::exec_sql(*in.conn, "SET bioseq.read_sequence_index   = off;");
        const std::string load_sql = fmt::format(
            "SELECT bioseq.load('{}'::regclass, '{}')",
            in.no_index_table, in.target_fasta);

        ScopedResourceMonitor resources;
        const auto start_ns = static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
        {
            auto _scope = bioseq::instr::region(kMeasuredRegion); // TIMED: load only
            it::exec_sql(*in.conn, load_sql);
        }
        const auto end_ns = static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
        attach_resource_metrics(out, start_ns, end_ns, resources.stop());
        out.found = true;
        out.match_count = 1;
        capture_index_audit(*in.conn, in.no_index_table, minimizer_index, out);
        log_measured_case(
            algo, pipeline_mode, minimap_threads, in, out, minimizer_index);

    } else if (algo == "bioseq_recompute_only") {
        const bool recompute_async_persist =
            param_string(params, "recompute_async_persist", "off") == "on";
        configure_bioseq_scheduler_session(*in.conn, pipeline_mode);
        configure_bioseq_pair_sort_session(*in.conn, params);
        it::exec_sqlf(*in.conn, "TRUNCATE {};", in.no_index_table);
        configure_bioseq_minimizer_policy(
            *in.conn, in.no_index_table, minimizer_index);
        it::exec_sql(*in.conn, "SET bioseq.update_index_on_insert = off;");
        it::exec_sql(*in.conn, "SET bioseq.read_sequence_index   = off;");
        it::exec_sqlf(*in.conn,
            "SELECT bioseq.load('{}'::regclass, '{}')",
            in.no_index_table, in.target_fasta);

        it::connection_test_context read_conn;
        configure_bioseq_aligner_session(read_conn, in.kmer_size, in.window_size);
        configure_bioseq_scheduler_session(read_conn, pipeline_mode);
        configure_bioseq_pair_sort_session(read_conn, params);
        it::exec_sql(read_conn, "SET bioseq.update_index_on_insert = off;");
        it::exec_sql(read_conn, "SET bioseq.read_sequence_index   = off;");
        it::exec_sqlf(read_conn, "SET bioseq.recompute_async_persist = {};",
                      recompute_async_persist ? "on" : "off");

        const std::string align_sql = fmt::format(
            "SELECT header, {} FROM {} WHERE mmsearch(seq, {}, "
            "aligner_options(index_read_requirement => 1)) ORDER BY header",
            proj, in.no_index_table, qlit);
        ScopedResourceMonitor resources;
        const auto start_ns = static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
        {
            auto _scope = bioseq::instr::region(kMeasuredRegion); // TIMED: recompute align only
            parse_bioseq_rows(it::query_rows(read_conn, align_sql), in, out);
            capture_mps_build_stats(read_conn, out);
        }
        const auto end_ns = static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
        attach_resource_metrics(out, start_ns, end_ns, resources.stop());
        require_mapped(algo, in, out);
        if (recompute_async_persist) {
            capture_index_audit_after_async_persist(
                *in.conn, in.no_index_table, minimizer_index, out);
            capture_mps_build_stats(*in.conn, out);
        } else {
            capture_index_audit(
                *in.conn, in.no_index_table, minimizer_index, out);
        }
        log_measured_case(
            algo, pipeline_mode, minimap_threads, in, out, minimizer_index);

    } else if (algo == "minimap2_no_index") {
        const std::string cmd = fmt::format(
            "{} -c --secondary=no -t{} -k{} -w{} {} {}",
            BIOSEQ_E2E_MINIMAP2_BIN, minimap_threads, in.kmer_size, in.window_size,
            in.target_fasta_split, in.query_fasta);
        ScopedResourceMonitor resources;
        const auto start_ns = static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
        {
            auto _scope = bioseq::instr::region(kMeasuredRegion); // TIMED: build + map
            parse_minimap2_paf(run_cmd_capture(cmd), in, out);
        }
        const auto end_ns = static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
        attach_resource_metrics(out, start_ns, end_ns, resources.stop());
        require_mapped(algo, in, out);
        log_measured_case(algo, pipeline_mode, minimap_threads, in, out);

    } else if (algo == "minimap2_just_index") {
        const std::string cmd = fmt::format(
            "{} -c --secondary=no -t{} {} {}",
            BIOSEQ_E2E_MINIMAP2_BIN, minimap_threads, in.mmi_path, in.query_fasta);
        ScopedResourceMonitor resources;
        const auto start_ns = static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
        {
            auto _scope = bioseq::instr::region(kMeasuredRegion); // TIMED: map only
            parse_minimap2_paf(run_cmd_capture(cmd), in, out);
        }
        const auto end_ns = static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
        attach_resource_metrics(out, start_ns, end_ns, resources.stop());
        require_mapped(algo, in, out);
        log_measured_case(algo, pipeline_mode, minimap_threads, in, out);

    } else {
        bioseq_log<ERR>("[bench_e2e] unknown algorithm '{}'", algo);
    }
    return out;
}

// ---- §10.4 on_evaluate ----
[[nodiscard]] std::map<std::string, double> e2e_eval(
    std::string_view, const ParamMap&, const E2eInput&, const E2eOutput& o) {
    std::map<std::string, double> metrics{
        {"match_count",             static_cast<double>(o.match_count)},
        {"expected_match_count",    static_cast<double>(o.expected_match_count)},
        {"correct_match_count",     static_cast<double>(o.correct_match_count)},
        {"exact_primary_count_accepted", static_cast<double>(o.exact_primary_count_accepted)},
        {"strand_result_count",     static_cast<double>(o.strand_result_count)},
        {"correct_strand_count",    static_cast<double>(o.correct_strand_count)},
        {"cigar_result_count",      static_cast<double>(o.cigar_result_count)},
        {"correct_cigar_count",     static_cast<double>(o.correct_cigar_count)},
        {"cigar_replay_count",      static_cast<double>(o.cigar_replay_count)},
        {"found_fraction",          o.found ? 1.0 : 0.0},
        {"mean_position_offset_bp", o.abs_offset_bp},
        {"max_position_offset_bp",  o.max_abs_offset_bp},
        {"pipeline_proof_present", static_cast<double>(o.pipeline_proof_present)},
        {"pipeline_proof_attached_to_measurement", static_cast<double>(o.pipeline_proof_attached_to_measurement)},
        {"pipeline_proof_accepted", static_cast<double>(o.pipeline_proof_accepted)},
        {"stage_engine_matrix_valid",
            static_cast<double>(o.stage_engine_matrix_valid)},
        {"effective_cpu_workers", static_cast<double>(o.effective_cpu_workers)},
        {"comparison_cpu_workers", static_cast<double>(o.comparison_cpu_workers)},
        {"worker_parity", (o.effective_cpu_workers < 0
            || o.effective_cpu_workers == o.comparison_cpu_workers) ? 1.0 : 0.0},
        {"alignment_scan_node_count", static_cast<double>(o.alignment_scan_node_count)},
        {"nested_alignment_executor_count", static_cast<double>(o.nested_alignment_executor_count)},
        {"alignment_completed", static_cast<double>(o.alignment_completed)},
        {"alignment_partial", static_cast<double>(o.alignment_partial)},
        {"alignment_fully_drained", static_cast<double>(o.alignment_fully_drained)},
        {"persisted_generation_id", static_cast<double>(o.persisted_generation_id)},
        {"pre_restart_persisted_gpu_abi_version",
            static_cast<double>(o.pre_restart_persisted_gpu_abi_version)},
        {"persisted_manifest_rows_read",
            static_cast<double>(o.persisted_manifest_rows_read)},
        {"persisted_manifest_bytes_read",
            static_cast<double>(o.persisted_manifest_bytes_read)},
        {"persisted_format5_manifest_contract_valid",
            static_cast<double>(o.persisted_format5_manifest_contract_valid)},
        {"persisted_generation_read_sessions",
            static_cast<double>(o.persisted_generation_read_sessions)},
        {"persisted_relation_opens",
            static_cast<double>(o.persisted_relation_opens)},
        {"persisted_index_scans",
            static_cast<double>(o.persisted_index_scans)},
        {"persisted_source_identity_present",
            o.persisted_source_identity.empty()
                || o.persisted_source_identity == "0000000000000000" ? 0.0 : 1.0},
        {"gpu_environment_present", o.gpu_device_uuid.empty() ? 0.0 : 1.0},
        {"gpu_pipeline_cache_seed_size",
            static_cast<double>(o.gpu_pipeline_cache_seed_size)},
        {"gpu_pipeline_cache_hit", static_cast<double>(o.gpu_pipeline_cache_hit)},
        {"requested_backend_scalar_cpu", o.requested_backend == "scalar_cpu" ? 1.0 : 0.0},
        {"requested_backend_simd_cpu", o.requested_backend == "simd_cpu" ? 1.0 : 0.0},
        {"requested_backend_vulkan_gpu", o.requested_backend == "vulkan_gpu" ? 1.0 : 0.0},
        {"requested_backend_mixed_invalid", o.requested_backend == "mixed_invalid" ? 1.0 : 0.0},
        {"actual_backend_scalar_cpu", o.actual_backend == "scalar_cpu" ? 1.0 : 0.0},
        {"actual_backend_simd_cpu", o.actual_backend == "simd_cpu" ? 1.0 : 0.0},
        {"actual_backend_vulkan_gpu", o.actual_backend == "vulkan_gpu" ? 1.0 : 0.0},
        {"actual_backend_mixed_invalid", o.actual_backend == "mixed_invalid" ? 1.0 : 0.0},
        {"requested_target_index_source_recomputed", o.requested_target_index_source == "recomputed" ? 1.0 : 0.0},
        {"requested_target_index_source_persisted", o.requested_target_index_source == "persisted" ? 1.0 : 0.0},
        {"requested_target_index_source_mixed_invalid", o.requested_target_index_source == "mixed_invalid" ? 1.0 : 0.0},
        {"actual_target_index_source_recomputed", o.actual_target_index_source == "recomputed" ? 1.0 : 0.0},
        {"actual_target_index_source_persisted", o.actual_target_index_source == "persisted" ? 1.0 : 0.0},
        {"actual_target_index_source_mixed_invalid", o.actual_target_index_source == "mixed_invalid" ? 1.0 : 0.0},
        {"simd_evidence_schema_valid", static_cast<double>(o.simd_evidence_schema_valid)},
        {"requested_simd_policy_none", o.requested_simd_chaining_policy == "none" ? 1.0 : 0.0},
        {"requested_simd_policy_exact_visited", o.requested_simd_chaining_policy == "exact_visited" ? 1.0 : 0.0},
        {"requested_simd_policy_bounded_batch_approximate", o.requested_simd_chaining_policy == "bounded_batch_approximate" ? 1.0 : 0.0},
        {"requested_simd_policy_mixed_invalid", o.requested_simd_chaining_policy == "mixed_invalid" ? 1.0 : 0.0},
        {"observed_simd_policy_none", o.observed_simd_chaining_policy == "none" ? 1.0 : 0.0},
        {"observed_simd_policy_exact_visited", o.observed_simd_chaining_policy == "exact_visited" ? 1.0 : 0.0},
        {"observed_simd_policy_bounded_batch_approximate", o.observed_simd_chaining_policy == "bounded_batch_approximate" ? 1.0 : 0.0},
        {"observed_simd_policy_mixed_invalid", o.observed_simd_chaining_policy == "mixed_invalid" ? 1.0 : 0.0},
        {"requested_simd_alignment_block_aligner", o.requested_simd_alignment_algorithm == "block_aligner" ? 1.0 : 0.0},
        {"observed_simd_alignment_block_aligner", o.observed_simd_alignment_algorithm == "block_aligner" ? 1.0 : 0.0},
        {"block_aligner_execution_proof_present", static_cast<double>(o.block_aligner_execution_proof_present)},
        {"block_aligner_execution_proof_accepted", static_cast<double>(o.block_aligner_execution_proof_accepted)},
        {"block_aligner_executed_calls", static_cast<double>(o.block_aligner_executed_calls)},
        {"block_aligner_nonempty_calls", static_cast<double>(o.block_aligner_nonempty_calls)},
        {"block_aligner_nonempty_calls_with_simd", static_cast<double>(o.block_aligner_nonempty_calls_with_simd)},
        {"block_aligner_simd_dp_vector_batches", static_cast<double>(o.block_aligner_simd_dp_vector_batches)},
        {"block_aligner_simd_dp_cells", static_cast<double>(o.block_aligner_simd_dp_cells)},
        {"block_aligner_forbidden_ksw2_calls", static_cast<double>(o.block_aligner_forbidden_ksw2_calls)},
        {"block_aligner_forbidden_scalar_biological_fallbacks", static_cast<double>(o.block_aligner_forbidden_scalar_biological_fallbacks)},
        {"block_aligner_execution_proof_saturated", static_cast<double>(o.block_aligner_execution_proof_saturated)},
        {"nthash_vector_batches", static_cast<double>(o.nthash_vector_batches)},
        {"nthash_resident_vector_stream_kernel_calls", static_cast<double>(o.nthash_resident_vector_stream_kernel_calls)},
        {"simd_chaining_exact_policy_calls", static_cast<double>(o.simd_chaining_exact_policy_calls)},
        {"simd_chaining_bounded_policy_calls", static_cast<double>(o.simd_chaining_bounded_policy_calls)},
        {"simd_chaining_bounded_score_batches", static_cast<double>(o.simd_chaining_bounded_score_batches)},
        {"simd_chaining_bounded_cutoff_stops", static_cast<double>(o.simd_chaining_bounded_cutoff_stops)},
        {"simd_chaining_bounded_candidates_skipped", static_cast<double>(o.simd_chaining_bounded_candidates_skipped)},
        {"simd_chaining_policy_rejections", static_cast<double>(o.simd_chaining_policy_rejections)},
        {"simd_chaining_policy_evidence_saturated", static_cast<double>(o.simd_chaining_policy_evidence_saturated)},
        {"attempted_alignment_jobs", static_cast<double>(o.attempted_alignment_jobs)},
        {"fully_attested_alignment_jobs", static_cast<double>(o.fully_attested_alignment_jobs)},
        {"unattested_alignment_jobs", static_cast<double>(o.unattested_alignment_jobs)},
        {"first_unattested_target_row", static_cast<double>(o.first_unattested_target_row)},
        {"first_unattested_query_ordinal", static_cast<double>(o.first_unattested_query_ordinal)},
        {"environment_mixed_invalid", static_cast<double>(o.environment_mixed_invalid)},
        {"mixed_persisted_identity", static_cast<double>(o.mixed_persisted_identity)},
        {"operational_identity_conflict", static_cast<double>(o.operational_identity_conflict)},
        {"stats_saturated", static_cast<double>(o.stats_saturated)},
        {"target_rows", static_cast<double>(o.target_rows)},
        {"persisted_index_rows", static_cast<double>(o.persisted_index_rows)},
        {"missing_persisted_index_rows", static_cast<double>(o.missing_persisted_index_rows)},
        {"recomputed_target_index_rows", static_cast<double>(o.recomputed_target_index_rows)},
        {"cold_index_schema_valid", static_cast<double>(o.cold_index_schema_valid)},
        {"cold_index_route_empty", static_cast<double>(o.cold_index_route_empty)},
        {"cold_index_route_selective", static_cast<double>(o.cold_index_route_selective)},
        {"cold_index_route_full", static_cast<double>(o.cold_index_route_full)},
        {"cold_index_payload_rows_requested", static_cast<double>(o.cold_index_payload_rows_requested)},
        {"cold_index_payload_rows_delivered", static_cast<double>(o.cold_index_payload_rows_delivered)},
        {"cold_index_payload_rows_scanned", static_cast<double>(o.cold_index_payload_rows_scanned)},
        {"cold_index_payload_rows_skipped", static_cast<double>(o.cold_index_payload_rows_skipped)},
        {"cold_index_payload_bytes_requested", static_cast<double>(o.cold_index_payload_bytes_requested)},
        {"cold_index_payload_bytes_delivered", static_cast<double>(o.cold_index_payload_bytes_delivered)},
        {"cold_index_payload_bytes_scanned", static_cast<double>(o.cold_index_payload_bytes_scanned)},
        {"extent_crc_raw_bytes_hashed", static_cast<double>(o.extent_crc_raw_bytes_hashed)},
        {"extent_crc_combined_bytes", static_cast<double>(o.extent_crc_combined_bytes)},
        {"extent_crc_combine_rows", static_cast<double>(o.extent_crc_combine_rows)},
        {"format5_reader_capacity_accounting_passes", static_cast<double>(o.format5_reader_capacity_accounting_passes)},
        {"format5_reader_global_position_storage_preparations", static_cast<double>(o.format5_reader_global_position_storage_preparations)},
        {"format5_reader_global_position_storage_allocations", static_cast<double>(o.format5_reader_global_position_storage_allocations)},
        {"format5_reader_position_append_growth_reservations", static_cast<double>(o.format5_reader_position_append_growth_reservations)},
        {"format5_reader_retained_position_count", static_cast<double>(o.format5_reader_retained_position_count)},
        {"format5_reader_retained_position_capacity", static_cast<double>(o.format5_reader_retained_position_capacity)},
        {"format5_reader_directory_capacity_bytes", static_cast<double>(o.format5_reader_directory_capacity_bytes)},
        {"format5_reader_query_hash_capacity_bytes", static_cast<double>(o.format5_reader_query_hash_capacity_bytes)},
        {"format5_reader_query_record_capacity_bytes", static_cast<double>(o.format5_reader_query_record_capacity_bytes)},
        {"format5_reader_ordinal_route_capacity_bytes", static_cast<double>(o.format5_reader_ordinal_route_capacity_bytes)},
        {"format5_reader_selected_extent_capacity_bytes", static_cast<double>(o.format5_reader_selected_extent_capacity_bytes)},
        {"format5_reader_current_selected_control_capacity_bytes", static_cast<double>(o.format5_reader_current_selected_control_capacity_bytes)},
        {"format5_reader_global_position_capacity_bytes", static_cast<double>(o.format5_reader_global_position_capacity_bytes)},
        {"format5_reader_control_payload_capacity_bytes", static_cast<double>(o.format5_reader_control_payload_capacity_bytes)},
        {"format5_reader_owned_capacity_bytes", static_cast<double>(o.format5_reader_owned_capacity_bytes)},
        {"format5_reader_admitted_bytes", static_cast<double>(o.format5_reader_admitted_bytes)},
        {"format5_reader_exact_position_image_checks", static_cast<double>(o.format5_reader_exact_position_image_checks)},
        {"format5_reader_exact_position_image_failures", static_cast<double>(o.format5_reader_exact_position_image_failures)},
        {"format5_reader_retained_layout_checks", static_cast<double>(o.format5_reader_retained_layout_checks)},
        {"format5_reader_retained_layout_failures", static_cast<double>(o.format5_reader_retained_layout_failures)},
        {"fragment_native_waves", static_cast<double>(o.fragment_native_waves)},
        {"fragment_native_queries", static_cast<double>(o.fragment_native_queries)},
        {"fragment_native_occurrence_fill_ns", static_cast<double>(o.fragment_native_occurrence_fill_ns)},
        {"fragment_native_occurrence_radix_ns", static_cast<double>(o.fragment_native_occurrence_radix_ns)},
        {"fragment_native_unique_plan_build_ns", static_cast<double>(o.fragment_native_unique_plan_build_ns)},
        {"fragment_native_query_plan_radix_fill_ns", static_cast<double>(o.fragment_native_query_plan_radix_fill_ns)},
        {"fragment_native_control_count_pass_ns", static_cast<double>(o.fragment_native_control_count_pass_ns)},
        {"fragment_native_rejected_control_validation_ns", static_cast<double>(o.fragment_native_rejected_control_validation_ns)},
        {"fragment_native_orientation_layout_ns", static_cast<double>(o.fragment_native_orientation_layout_ns)},
        {"fragment_native_direct_emission_ns", static_cast<double>(o.fragment_native_direct_emission_ns)},
        {"fragment_native_local_sort_ns", static_cast<double>(o.fragment_native_local_sort_ns)},
        {"fragment_native_tie_normalization_ns", static_cast<double>(o.fragment_native_tie_normalization_ns)},
        {"fragment_native_control_count_bytes", static_cast<double>(o.fragment_native_control_count_bytes)},
        {"fragment_native_rejected_control_validation_bytes", static_cast<double>(o.fragment_native_rejected_control_validation_bytes)},
        {"fragment_native_pass2_position_bytes", static_cast<double>(o.fragment_native_pass2_position_bytes)},
        {"fragment_native_pass3_position_bytes", static_cast<double>(o.fragment_native_pass3_position_bytes)},
        {"fragment_native_anchors_constructed", static_cast<double>(o.fragment_native_anchors_constructed)},
        {"fragment_native_local_sort_bytes", static_cast<double>(o.fragment_native_local_sort_bytes)},
        {"fragment_native_tie_normalization_bytes", static_cast<double>(o.fragment_native_tie_normalization_bytes)},
        {"fragment_native_exact_final_bytes", static_cast<double>(o.fragment_native_exact_final_bytes)},
        {"fragment_native_peak_exact_final_bytes", static_cast<double>(o.fragment_native_peak_exact_final_bytes)},
        {"fragment_native_global_sort_calls_invoked", static_cast<double>(o.fragment_native_global_sort_calls_invoked)},
        {"fragment_native_global_sort_calls_skipped", static_cast<double>(o.fragment_native_global_sort_calls_skipped)},
        {"fragment_native_candidate_anchor_bytes", static_cast<double>(o.fragment_native_candidate_anchor_bytes)},
        {"fragment_native_plan_ordinal_bytes", static_cast<double>(o.fragment_native_plan_ordinal_bytes)},
        {"fragment_native_compaction_bytes", static_cast<double>(o.fragment_native_compaction_bytes)},
        {"fragment_native_final_copy_bytes", static_cast<double>(o.fragment_native_final_copy_bytes)},
        {"fragment_native_peak_occurrence_capacity_bytes", static_cast<double>(o.fragment_native_peak_occurrence_capacity_bytes)},
        {"fragment_native_peak_query_plan_capacity_bytes", static_cast<double>(o.fragment_native_peak_query_plan_capacity_bytes)},
        {"fragment_native_peak_query_group_offset_capacity_bytes", static_cast<double>(o.fragment_native_peak_query_group_offset_capacity_bytes)},
        {"fragment_native_peak_query_membership_filter_capacity_bytes", static_cast<double>(o.fragment_native_peak_query_membership_filter_capacity_bytes)},
        {"fragment_native_peak_query_radix_capacity_bytes", static_cast<double>(o.fragment_native_peak_query_radix_capacity_bytes)},
        {"fragment_native_peak_occurrence_count_capacity_bytes", static_cast<double>(o.fragment_native_peak_occurrence_count_capacity_bytes)},
        {"fragment_native_peak_block_layout_capacity_bytes", static_cast<double>(o.fragment_native_peak_block_layout_capacity_bytes)},
        {"fragment_native_peak_block_evidence_capacity_bytes", static_cast<double>(o.fragment_native_peak_block_evidence_capacity_bytes)},
        {"fragment_native_occurrence_allocations", static_cast<double>(o.fragment_native_occurrence_allocations)},
        {"fragment_native_query_plan_allocations", static_cast<double>(o.fragment_native_query_plan_allocations)},
        {"fragment_native_query_group_offset_allocations", static_cast<double>(o.fragment_native_query_group_offset_allocations)},
        {"fragment_native_query_membership_filter_allocations", static_cast<double>(o.fragment_native_query_membership_filter_allocations)},
        {"fragment_native_query_radix_allocations", static_cast<double>(o.fragment_native_query_radix_allocations)},
        {"fragment_native_occurrence_count_allocations", static_cast<double>(o.fragment_native_occurrence_count_allocations)},
        {"fragment_native_block_layout_allocations", static_cast<double>(o.fragment_native_block_layout_allocations)},
        {"fragment_native_block_evidence_allocations", static_cast<double>(o.fragment_native_block_evidence_allocations)},
        {"fragment_native_query_occurrences", static_cast<double>(o.fragment_native_query_occurrences)},
        {"fragment_native_unique_query_plans", static_cast<double>(o.fragment_native_unique_query_plans)},
        {"fragment_native_packed_occurrence_waves", static_cast<double>(o.fragment_native_packed_occurrence_waves)},
        {"fragment_native_wide_occurrence_waves", static_cast<double>(o.fragment_native_wide_occurrence_waves)},
        {"fragment_native_compact_direct_lookup_ns", static_cast<double>(o.fragment_native_compact_direct_lookup_ns)},
        {"fragment_native_compact_direct_selection_shards", static_cast<double>(o.fragment_native_compact_direct_selection_shards)},
        {"fragment_native_compact_direct_shard_query_layouts", static_cast<double>(o.fragment_native_compact_direct_shard_query_layouts)},
        {"fragment_native_compact_direct_layout_shards_completed", static_cast<double>(o.fragment_native_compact_direct_layout_shards_completed)},
        {"fragment_native_compact_direct_emission_shards_completed", static_cast<double>(o.fragment_native_compact_direct_emission_shards_completed)},
        {"fragment_native_compact_direct_posting_visit_passes", static_cast<double>(o.fragment_native_compact_direct_posting_visit_passes)},
        {"fragment_native_compact_direct_selected_postings", static_cast<double>(o.fragment_native_compact_direct_selected_postings)},
        {"fragment_native_compact_direct_layout_postings_visited", static_cast<double>(o.fragment_native_compact_direct_layout_postings_visited)},
        {"fragment_native_compact_direct_emission_postings_visited", static_cast<double>(o.fragment_native_compact_direct_emission_postings_visited)},
        {"fragment_native_compact_direct_target_control_scan_bytes", static_cast<double>(o.fragment_native_compact_direct_target_control_scan_bytes)},
        {"fragment_native_compact_direct_target_control_scan_passes", static_cast<double>(o.fragment_native_compact_direct_target_control_scan_passes)},
        {"fragment_native_compact_direct_fallbacks", static_cast<double>(o.fragment_native_compact_direct_fallbacks)},
        {"fragment_native_compact_direct_traversal_rank_checkpoints_examined", static_cast<double>(o.fragment_native_compact_direct_traversal_rank_checkpoints_examined)},
        {"fragment_native_compact_direct_traversal_membership_words_examined", static_cast<double>(o.fragment_native_compact_direct_traversal_membership_words_examined)},
        {"fragment_native_compact_direct_rejected_posting_reads", static_cast<double>(o.fragment_native_compact_direct_rejected_posting_reads)},
        {"fragment_native_evidence_saturated", static_cast<double>(o.fragment_native_evidence_saturated)},
        {"format5_reader_memory_evidence_saturated", static_cast<double>(o.format5_reader_memory_evidence_saturated)},
        {"persisted_format_version", static_cast<double>(o.persisted_format_version)},
        {"persisted_format6_contract_valid", static_cast<double>(o.persisted_format6_contract_valid)},
        {"format6_restart_audit_accepted", static_cast<double>(o.format6_restart_audit_accepted)},
        {"format6_directory_rows", static_cast<double>(o.format6_directory_rows)},
        {"format6_directory_bytes", static_cast<double>(o.format6_directory_bytes)},
        {"format6_control_rows", static_cast<double>(o.format6_control_rows)},
        {"format6_control_bytes", static_cast<double>(o.format6_control_bytes)},
        {"format6_posting_rows", static_cast<double>(o.format6_posting_rows)},
        {"format6_posting_bytes", static_cast<double>(o.format6_posting_bytes)},
        {"format6_posting_ranges_delivered", static_cast<double>(o.format6_posting_ranges_delivered)},
        {"format6_selected_block_count", static_cast<double>(o.format6_selected_block_count)},
        {"format6_accepted_hash_count", static_cast<double>(o.format6_accepted_hash_count)},
        {"format6_retained_position_count", static_cast<double>(o.format6_retained_position_count)},
        {"format6_authorized_posting_bytes", static_cast<double>(o.format6_authorized_posting_bytes)},
        {"format6_peak_admitted_bytes", static_cast<double>(o.format6_peak_admitted_bytes)},
        {"format6_retained_owner_bytes", static_cast<double>(o.format6_retained_owner_bytes)},
        {"format6_admitted_bytes", static_cast<double>(o.format6_admitted_bytes)},
        {"format6_failures", static_cast<double>(o.format6_failures)},
        {"format6_saturated", static_cast<double>(o.format6_saturated)},
        {"gpu_seed_dispatches", static_cast<double>(o.gpu_seed_dispatches)},
        {"gpu_chain_dispatches", static_cast<double>(o.gpu_chain_dispatches)},
        {"gpu_chain_backtrack_dispatches", static_cast<double>(o.gpu_chain_backtrack_dispatches)},
        {"gpu_extend_dispatches", static_cast<double>(o.gpu_extend_dispatches)},
        {"backend_declines", static_cast<double>(o.backend_declines)},
        {"backend_fallbacks", static_cast<double>(o.backend_fallbacks)},
        {"query_sketch_host_wall_ns", static_cast<double>(o.query_sketch_host_wall_ns)},
        {"target_index_build_host_wall_ns", static_cast<double>(o.target_index_build_host_wall_ns)},
        {"host_query_hash_readbacks", static_cast<double>(o.host_query_hash_readbacks)},
        {"host_query_symbol_packing_calls", static_cast<double>(o.host_query_symbol_packing_calls)},
        {"host_target_symbol_packing_calls", static_cast<double>(o.host_target_symbol_packing_calls)},
        {"host_alignment_job_classification_calls", static_cast<double>(o.host_alignment_job_classification_calls)},
        {"host_cigar_construction_calls", static_cast<double>(o.host_cigar_construction_calls)},
        {"measured_region_ns_manual", static_cast<double>(o.measured_region_ns)},
        {"peak_postgres_rss_mb",    static_cast<double>(o.peak_postgres_rss_kb) / 1024.0},
        {"peak_minimap2_rss_mb",    static_cast<double>(o.peak_minimap2_rss_kb) / 1024.0},
        {"peak_bench_rss_mb",       static_cast<double>(o.peak_bench_rss_kb) / 1024.0},
        {"min_tmp_available_gib",   static_cast<double>(o.min_tmp_available_kb) / (1024.0 * 1024.0)},
        {"peak_swap_used_mb",       o.peak_swap_used_mb},
        {"just_index_restart_performed", static_cast<double>(o.just_index_restart_performed)},
        {"just_index_restart_identity_changed",
            static_cast<double>(o.cold_restart_identity_changed)},
        {"cold_no_index_restart_performed", static_cast<double>(o.cold_no_index_restart_performed)},
        {"cold_no_index_restart_identity_changed", static_cast<double>(o.cold_no_index_restart_identity_changed)},
        {"cold_no_index_pre_restart_target_rows", static_cast<double>(o.cold_no_index_pre_restart_target_rows)},
        {"cold_no_index_post_restart_target_rows", static_cast<double>(o.cold_no_index_post_restart_target_rows)},
        {"cold_no_index_pre_restart_mpf_rows", static_cast<double>(o.cold_no_index_pre_restart_mpf_rows)},
        {"cold_no_index_pre_restart_mps_rows", static_cast<double>(o.cold_no_index_pre_restart_mps_rows)},
        {"cold_no_index_post_restart_mpf_rows", static_cast<double>(o.cold_no_index_post_restart_mpf_rows)},
        {"cold_no_index_post_restart_mps_rows", static_cast<double>(o.cold_no_index_post_restart_mps_rows)},
        {"cold_no_index_pre_restart_async_jobs", static_cast<double>(o.cold_no_index_pre_restart_async_jobs)},
        {"cold_no_index_post_restart_async_jobs", static_cast<double>(o.cold_no_index_post_restart_async_jobs)},
        {"cold_no_index_pre_restart_gucs_off", static_cast<double>(o.cold_no_index_pre_restart_gucs_off)},
        {"cold_no_index_post_restart_gucs_off", static_cast<double>(o.cold_no_index_post_restart_gucs_off)},
        {"cold_no_index_lifecycle_accepted", static_cast<double>(o.cold_no_index_lifecycle_accepted)},
        {"persisted_policy_audit_accepted",
            static_cast<double>(o.persisted_policy_audit_accepted)},
        {"audit_selected_minimizer_is_sequence", static_cast<double>(o.audit_selected_minimizer_is_sequence)},
        {"audit_mpf_rows",          static_cast<double>(o.audit_mpf_rows)},
        {"audit_mps_rows",          static_cast<double>(o.audit_mps_rows)},
        {"audit_mpf_update_on_insert", static_cast<double>(o.audit_mpf_update_on_insert)},
        {"audit_mps_update_on_insert", static_cast<double>(o.audit_mps_update_on_insert)},
        {"audit_mpf_active_read",   static_cast<double>(o.audit_mpf_active_read)},
        {"audit_mps_active_read",   static_cast<double>(o.audit_mps_active_read)},
        {"audit_async_persist_jobs_waiting", static_cast<double>(o.audit_async_persist_jobs_waiting)},
        {"audit_index_rows_wait_attempts", static_cast<double>(o.audit_index_rows_wait_attempts)},
        {"mps_build_backend_pid", static_cast<double>(o.mps_build_backend_pid)},
        {"mps_build_db_oid", static_cast<double>(o.mps_build_db_oid)},
        {"mps_build_relfilenode", static_cast<double>(o.mps_build_relfilenode)},
        {"mps_build_sequence_rowno", static_cast<double>(o.mps_build_sequence_rowno)},
        {"mps_build_total_run_count", static_cast<double>(o.mps_build_total_run_count)},
        {"mps_build_resident_runs", static_cast<double>(o.mps_build_resident_runs)},
        {"mps_build_file_runs", static_cast<double>(o.mps_build_file_runs)},
        {"mps_build_resident_run_bytes", static_cast<double>(o.mps_build_resident_run_bytes)},
        {"mps_build_resident_peak_bytes", static_cast<double>(o.mps_build_resident_peak_bytes)},
        {"mps_build_spilled_run_bytes", static_cast<double>(o.mps_build_spilled_run_bytes)},
        {"mps_build_postgres_temp_file_bytes", static_cast<double>(o.mps_build_postgres_temp_file_bytes)},
        {"mps_build_postgres_temp_file_count", static_cast<double>(o.mps_build_postgres_temp_file_count)},
        {"mps_build_merge_reader_buffer_bytes", static_cast<double>(o.mps_build_merge_reader_buffer_bytes)},
        {"mps_build_merge_pass_count", static_cast<double>(o.mps_build_merge_pass_count)},
        {"mps_build_spill_count_from_pressure", static_cast<double>(o.mps_build_spill_count_from_pressure)},
        {"mps_build_resident_demotions", static_cast<double>(o.mps_build_resident_demotions)},
        {"mps_build_position_spill_bytes", static_cast<double>(o.mps_build_position_spill_bytes)},
        {"mps_build_position_spill_count", static_cast<double>(o.mps_build_position_spill_count)},
        {"mps_build_shared_generation", static_cast<double>(o.mps_build_shared_generation)},
        {"mps_build_worker_build_ns", static_cast<double>(o.mps_build_worker_build_ns)},
        {"mps_build_pipeline_mode", static_cast<double>(o.mps_build_pipeline_mode)},
        {"mps_build_max_concurrency", static_cast<double>(o.mps_build_max_concurrency)},
        {"mps_build_index_memory_cap_mb", static_cast<double>(o.mps_build_index_memory_cap_mb)},
        {"mps_build_lazy_worker_shareable", static_cast<double>(o.mps_build_lazy_worker_shareable)},
    };
    const bool format5_memory_v6_valid = json_has_exact_keys(
            o.format5_reader_memory_evidence,
            kFormat5ReaderMemoryEvidenceV6Keys)
        && o.format5_reader_memory_evidence["version"].is_number_integer()
        && o.format5_reader_memory_evidence["version"] == 6;
    for (const std::string_view key
         : kFormat5ReaderMemoryEvidenceV4AdditionKeys) {
        const double value = format5_memory_v6_valid
            ? static_cast<double>(required_json_counter(
                o.format5_reader_memory_evidence, key))
            : -1.0;
        metrics.emplace(std::string{key}, value);
    }
    return metrics;
}

[[noreturn]] void abort_raw_evidence_serialization(
        std::string_view reason) {
    std::fprintf(
        stderr,
        "[bench_e2e] RAW-EVIDENCE-SERIALIZATION-FAIL %.*s\n",
        static_cast<int>(reason.size()),
        reason.data());
    std::fflush(stderr);
    std::abort();
}

[[nodiscard]] nlohmann::json serialize_alignment_snapshot_rows(
        const E2eOutput& out) {
    if (out.alignment_snapshot_rows.size()
        != E2eOutput::kAlignmentSnapshotRowCount) {
        abort_raw_evidence_serialization(
            "PostgreSQL sample is missing the exact 20-row alignment SRF");
    }

    nlohmann::json snapshot = nlohmann::json::object();
    snapshot["schema"] =
        "bioseq.last_alignment_execution_stats_v2/raw-rowset";
    snapshot["schema_version"] = 2;
    snapshot["expected_row_count"] =
        E2eOutput::kAlignmentSnapshotRowCount;
    snapshot["actual_row_count"] = out.alignment_snapshot_rows.size();
    snapshot["expected_column_count"] =
        E2eOutput::kAlignmentSnapshotColumnCount;

    nlohmann::json columns = nlohmann::json::array();
    for (std::size_t column = 0;
         column < kAlignmentSnapshotColumnNames.size(); ++column) {
        const std::string_view json_type = alignment_stats_json_column(column)
            ? std::string_view{"object"}
            : alignment_stats_text_column(column)
                ? std::string_view{"string"}
                : alignment_stats_boolean_column(column)
                    ? std::string_view{"boolean"}
                    : std::string_view{"integer"};
        columns.push_back(nlohmann::json{
            {"ordinal", column},
            {"name", kAlignmentSnapshotColumnNames[column]},
            {"json_type", json_type},
        });
    }
    snapshot["columns"] = std::move(columns);

    nlohmann::json serialized_rows = nlohmann::json::array();
    for (const auto& row : out.alignment_snapshot_rows) {
        if (row.size() != E2eOutput::kAlignmentSnapshotColumnCount) {
            abort_raw_evidence_serialization(
				"alignment SRF v2 row does not have exactly 151 columns");
        }
        nlohmann::json serialized_row = nlohmann::json::array();
        for (std::size_t column = 0; column < row.size(); ++column) {
            const auto& cell = row[column];
            if (!cell.has_value()) {
                serialized_row.push_back(nullptr);
                continue;
            }
            if (alignment_stats_json_column(column)) {
                try {
                    serialized_row.push_back(nlohmann::json::parse(*cell));
                } catch (const nlohmann::json::exception&) {
                    abort_raw_evidence_serialization(
                        "alignment SRF JSON changed after validation");
                }
                continue;
            }
            if (alignment_stats_text_column(column)) {
                serialized_row.push_back(*cell);
                continue;
            }
            if (alignment_stats_boolean_column(column)) {
                const auto parsed = parse_snapshot_bool(cell);
                if (!parsed.has_value()) {
                    abort_raw_evidence_serialization(
                        "alignment SRF Boolean changed after validation");
                }
                serialized_row.push_back(*parsed != 0);
                continue;
            }
            const auto parsed = parse_snapshot_i64(cell);
            if (!parsed.has_value()) {
                abort_raw_evidence_serialization(
                    "alignment SRF integer changed after validation");
            }
            serialized_row.push_back(*parsed);
        }
        serialized_rows.push_back(std::move(serialized_row));
    }
    snapshot["rows"] = std::move(serialized_rows);
    return snapshot;
}

[[nodiscard]] std::string serialize_e2e_raw_sample(
        std::string_view algorithm,
        const ParamMap&,
        const E2eInput& in,
        const E2eOutput& out) {
    const bool postgres_sample = algorithm.starts_with("bioseq_")
        && (algorithm.ends_with("_no_index")
            || algorithm.ends_with("_just_index"));
    const bool attested_backend_sample =
        algorithm.find("_scalar_") != std::string_view::npos
        || algorithm.find("_simd_") != std::string_view::npos
        || algorithm.find("_gpu_") != std::string_view::npos
        || algorithm.find("_vulkan_") != std::string_view::npos;
    const bool persisted_gpu_sample = postgres_sample
        && algorithm.ends_with("_just_index")
        && (algorithm.find("_gpu_") != std::string_view::npos
            || algorithm.find("_vulkan_") != std::string_view::npos);
    if (postgres_sample
        && out.alignment_snapshot_rows.size()
            != E2eOutput::kAlignmentSnapshotRowCount) {
        abort_raw_evidence_serialization(
            "bioseq sample has no complete same-invocation SRF evidence");
    }
    if (attested_backend_sample
        && out.pipeline_proof_accepted != 1) {
        abort_raw_evidence_serialization(
            "selected backend sample was not accepted before serialization");
    }
    if (postgres_sample
        && (out.persisted_gpu_operation_evidence_schema_valid != 1
            || out.persisted_gpu_operation_evidence.is_null())) {
        abort_raw_evidence_serialization(
            "PostgreSQL sample is missing versioned GPU operation evidence");
    }
    const bool format5_memory_v6_valid = json_has_exact_keys(
            out.format5_reader_memory_evidence,
            kFormat5ReaderMemoryEvidenceV6Keys)
        && out.format5_reader_memory_evidence["schema"].is_string()
        && out.format5_reader_memory_evidence["schema"]
            == "bioseq.format5_reader_memory_evidence"
        && out.format5_reader_memory_evidence["version"].is_number_integer()
        && out.format5_reader_memory_evidence["version"] == 6;
    if (postgres_sample && !format5_memory_v6_valid) {
        abort_raw_evidence_serialization(
            "PostgreSQL sample is missing exact Format-5 memory evidence v6");
    }
    const bool format6_reader_v1_valid = json_has_exact_keys(
            out.format6_reader_evidence,
            kFormat6ReaderEvidenceKeys)
        && out.format6_reader_evidence["schema"].is_string()
        && out.format6_reader_evidence["schema"]
            == "bioseq.format6_reader_evidence"
        && out.format6_reader_evidence["version"].is_number_integer()
        && out.format6_reader_evidence["version"] == 1;
    if (postgres_sample && !format6_reader_v1_valid) {
        abort_raw_evidence_serialization(
            "PostgreSQL sample is missing exact Format-6 reader evidence v1");
    }
    if (persisted_gpu_sample
        && out.persisted_gpu_operation_evidence_accepted != 1) {
        abort_raw_evidence_serialization(
            "persisted all-GPU sample lacks accepted operation evidence");
    }
    if (postgres_sample && algorithm.ends_with("_no_index")
        && out.cold_no_index_lifecycle_accepted != 1) {
        abort_raw_evidence_serialization(
            "no_index sample lacks accepted cold lifecycle evidence");
    }
    if (postgres_sample && algorithm.ends_with("_just_index")
        && out.persisted_policy_audit_accepted != 1) {
        abort_raw_evidence_serialization(
            "just_index sample lacks accepted post-measure persisted-policy evidence");
    }

    nlohmann::json payload = nlohmann::json::object();
    payload["payload_schema"] = "bioseq.e2e.raw-sample";
    payload["payload_schema_version"] = 6;
    payload["index_and_map_ns"] = out.measured_region_ns;
    payload["cache_policy"] = "process-cold-os-cache-unspecified";
    payload["benchmark_shape"] = nlohmann::json{
        {"expected_target_rows", postgres_sample
            ? nlohmann::json{1} : nlohmann::json{nullptr}},
        {"expected_query_rows", in.queries.size()},
        {"expected_alignment_jobs", in.queries.size()},
        {"actual_target_rows", postgres_sample
            ? nlohmann::json{out.target_rows} : nlohmann::json{nullptr}},
        {"actual_alignment_jobs", postgres_sample
            ? nlohmann::json{out.attempted_alignment_jobs}
            : nlohmann::json{nullptr}},
    };
    payload["correctness"] = nlohmann::json{
        {"found", out.found},
        {"match_count", out.match_count},
        {"expected_match_count", out.expected_match_count},
        {"correct_match_count", out.correct_match_count},
        {"exact_primary_count_accepted", out.exact_primary_count_accepted},
        {"strand_result_count", out.strand_result_count},
        {"correct_strand_count", out.correct_strand_count},
        {"cigar_result_count", out.cigar_result_count},
        {"correct_cigar_count", out.correct_cigar_count},
        {"cigar_replay_count", out.cigar_replay_count},
        {"max_abs_offset_bp", out.max_abs_offset_bp},
    };
    const auto absence_evidence_json = [](
            const ColdNoIndexPolicyAbsenceEvidence& evidence) {
        return nlohmann::json{
            {"target_rows", evidence.target_rows},
            {"target_cardinality_metadata_only",
                evidence.target_cardinality_metadata_only},
            {"source_parent_rows_read", evidence.source_parent_rows_read},
            {"source_fragment_rows_read", evidence.source_fragment_rows_read},
            {"source_decoded_bp", evidence.source_decoded_bp},
            {"physical_index_relations", evidence.physical_index_relations},
            {"persisted_index_rows", evidence.persisted_index_rows},
            {"generation_objects", evidence.generation_objects},
        };
    };

    if (postgres_sample) {
        const bool just_index = algorithm.ends_with("_just_index");
        const bool no_index = algorithm.ends_with("_no_index");
        const std::int64_t expected_generation = just_index
            ? out.pre_restart_persisted_generation_id : 0;
        const std::string expected_source = just_index
            ? out.pre_restart_persisted_source_identity
            : std::string{"0000000000000000"};
        const std::string& restart_before_start = just_index
            ? out.pre_restart_postmaster_start_time
            : out.cold_no_index_pre_restart_postmaster_start_time;
        const std::string& restart_after_start = just_index
            ? out.post_restart_postmaster_start_time
            : out.cold_no_index_post_restart_postmaster_start_time;
        const std::int64_t restart_before_pid = just_index
            ? out.pre_restart_backend_pid
            : out.cold_no_index_pre_restart_backend_pid;
        const std::int64_t restart_after_pid = just_index
            ? out.post_restart_backend_pid
            : out.cold_no_index_post_restart_backend_pid;
        const std::int64_t restart_before_database_oid = just_index
            ? out.pre_restart_database_oid
            : out.cold_no_index_pre_restart_database_oid;
        const std::int64_t restart_after_database_oid = just_index
            ? out.post_restart_database_oid
            : out.cold_no_index_post_restart_database_oid;
        const nlohmann::json restart_identity = nlohmann::json{
            {"before", nlohmann::json{
                {"postmaster_start", restart_before_start},
                {"backend_pid", restart_before_pid},
                {"database_oid", restart_before_database_oid},
            }},
            {"after", nlohmann::json{
                {"postmaster_start", restart_after_start},
                {"backend_pid", restart_after_pid},
                {"database_oid", restart_after_database_oid},
            }},
            {"timed_statement", nlohmann::json{
                {"postmaster_start", out.timed_postmaster_start_time},
                {"backend_pid", out.alignment_backend_pid},
                {"database_oid", out.alignment_database_oid},
            }},
        };
        const nlohmann::json persisted_policy = just_index
            ? nlohmann::json{
                {"accepted", out.persisted_policy_audit_accepted},
                {"captured_after_timed_statement", true},
                {"selected_index",
                    out.audit_selected_minimizer_is_sequence == 1
                        ? "minimizers_per_sequence"
                        : "minimizers_per_fragment"},
                {"selected_minimizer_is_sequence",
                    out.audit_selected_minimizer_is_sequence},
                {"minimizers_per_fragment", nlohmann::json{
                    {"rows", out.audit_mpf_rows},
                    {"update_on_insert", out.audit_mpf_update_on_insert},
                    {"active_read", out.audit_mpf_active_read},
                }},
                {"minimizers_per_sequence", nlohmann::json{
                    {"rows", out.audit_mps_rows},
                    {"update_on_insert", out.audit_mps_update_on_insert},
                    {"active_read", out.audit_mps_active_read},
                }},
                {"async_persist_jobs_waiting",
                    out.audit_async_persist_jobs_waiting},
                {"index_rows_wait_attempts", out.audit_index_rows_wait_attempts},
            }
            : nlohmann::json{nullptr};
        payload["postgres"] = nlohmann::json{
            {"postmaster_start", out.timed_postmaster_start_time},
            {"backend_pid", out.alignment_backend_pid},
            {"database_oid", out.alignment_database_oid},
            {"prior_execution_id", out.prior_alignment_execution_id},
            {"actual_execution_id", out.alignment_execution_id},
            {"expected_generation", expected_generation},
            {"expected_source_identity", expected_source},
            {"actual_generation", out.persisted_generation_id},
            {"actual_source_identity", out.persisted_source_identity},
            {"restart_identity", restart_identity},
            {"persisted_policy", persisted_policy},
            {"persisted_format", just_index
                ? nlohmann::json{
                    {"format_version", out.persisted_format_version},
                    {"pre_restart_format_version",
                        out.pre_restart_persisted_format_version},
                    {"post_restart_format_version",
                        out.post_restart_persisted_format_version},
                    {"ready_generation_gpu_abi_version",
                        out.pre_restart_persisted_gpu_abi_version},
                    {"manifest_rows_read",
                        out.persisted_manifest_rows_read},
                    {"manifest_bytes_read",
                        out.persisted_manifest_bytes_read},
                    {"read_sessions",
                        out.persisted_generation_read_sessions},
                    {"format5_manifest_contract_valid",
                        out.persisted_format5_manifest_contract_valid},
                    {"format6_contract_valid",
                        out.persisted_format6_contract_valid},
                    {"format6_restart_audit_accepted",
                        out.format6_restart_audit_accepted},
                    {"format6_pre_restart_ready_audit",
                        out.pre_restart_format6_ready_audit},
                    {"format6_post_restart_ready_audit",
                        out.post_restart_format6_ready_audit},
                }
                : nlohmann::json{nullptr}},
            {"restart_performed", just_index
                ? out.just_index_restart_performed
                : out.cold_no_index_restart_performed},
            {"restart_identity_changed", just_index
                ? out.cold_restart_identity_changed
                : out.cold_no_index_restart_identity_changed},
            {"cold_no_index_lifecycle", no_index
                ? nlohmann::json{
                    {"accepted", out.cold_no_index_lifecycle_accepted},
                    {"pre_restart_target_rows",
                        out.cold_no_index_pre_restart_target_rows},
                    {"post_restart_target_rows",
                        out.cold_no_index_post_restart_target_rows},
                    {"pre_restart_mpf_rows",
                        out.cold_no_index_pre_restart_mpf_rows},
                    {"pre_restart_mps_rows",
                        out.cold_no_index_pre_restart_mps_rows},
                    {"post_restart_mpf_rows",
                        out.cold_no_index_post_restart_mpf_rows},
                    {"post_restart_mps_rows",
                        out.cold_no_index_post_restart_mps_rows},
                    {"post_measure_mpf_rows",
                        out.cold_no_index_measured_mpf.persisted_index_rows},
                    {"post_measure_mps_rows",
                        out.cold_no_index_measured_mps.persisted_index_rows},
                    {"pre_restart_async_jobs",
                        out.cold_no_index_pre_restart_async_jobs},
                    {"post_restart_async_jobs",
                        out.cold_no_index_post_restart_async_jobs},
                    {"post_measure_async_jobs",
                        out.cold_no_index_measured_async_jobs},
                    {"pre_restart_gucs_off",
                        out.cold_no_index_pre_restart_gucs_off},
                    {"post_restart_gucs_off",
                        out.cold_no_index_post_restart_gucs_off},
                    {"post_measure_gucs_off",
                        out.cold_no_index_measured_gucs_off},
                    {"pre_restart_mpf_absence", absence_evidence_json(
                        out.cold_no_index_pre_restart_mpf)},
                    {"pre_restart_mps_absence", absence_evidence_json(
                        out.cold_no_index_pre_restart_mps)},
                    {"post_restart_mpf_absence", absence_evidence_json(
                        out.cold_no_index_post_restart_mpf)},
                    {"post_restart_mps_absence", absence_evidence_json(
                        out.cold_no_index_post_restart_mps)},
                    {"post_measure_mpf_absence", absence_evidence_json(
                        out.cold_no_index_measured_mpf)},
                    {"post_measure_mps_absence", absence_evidence_json(
                        out.cold_no_index_measured_mps)},
                }
                : nlohmann::json{nullptr}},
        };
        payload["alignment_snapshot"] =
            serialize_alignment_snapshot_rows(out);
        payload["persisted_gpu_operation_evidence"] =
            out.persisted_gpu_operation_evidence;
    } else {
        payload["postgres"] = nullptr;
        payload["alignment_snapshot"] = nullptr;
        payload["persisted_gpu_operation_evidence"] = nullptr;
    }
    payload["attestation"] = nlohmann::json{
        {"proof_present", out.pipeline_proof_present},
        {"proof_attached_to_measurement",
            out.pipeline_proof_attached_to_measurement},
        {"proof_accepted", out.pipeline_proof_accepted},
        {"stage_engine_matrix_valid", out.stage_engine_matrix_valid},
        {"requested_backend", out.requested_backend},
        {"actual_backend", out.actual_backend},
        {"requested_target_index_source",
            out.requested_target_index_source},
        {"actual_target_index_source", out.actual_target_index_source},
        {"requested_simd_chaining_policy",
            out.requested_simd_chaining_policy},
        {"observed_simd_chaining_policy",
            out.observed_simd_chaining_policy},
        {"requested_simd_alignment_algorithm",
            out.requested_simd_alignment_algorithm},
        {"observed_simd_alignment_algorithm",
            out.observed_simd_alignment_algorithm},
        {"nthash_vector_activity",
            out.nthash_vector_batches > 0
                || out.nthash_resident_vector_stream_kernel_calls > 0},
        {"block_aligner_execution_proof", nlohmann::json{
            {"present", out.block_aligner_execution_proof_present},
            {"accepted", out.block_aligner_execution_proof_accepted},
            {"executed_calls", out.block_aligner_executed_calls},
            {"nonempty_calls", out.block_aligner_nonempty_calls},
            {"nonempty_calls_with_simd",
                out.block_aligner_nonempty_calls_with_simd},
            {"simd_dp_vector_batches",
                out.block_aligner_simd_dp_vector_batches},
            {"simd_dp_cells", out.block_aligner_simd_dp_cells},
            {"forbidden_ksw2_calls",
                out.block_aligner_forbidden_ksw2_calls},
            {"forbidden_scalar_biological_fallbacks",
                out.block_aligner_forbidden_scalar_biological_fallbacks},
            {"saturated", out.block_aligner_execution_proof_saturated},
        }},
        {"backend_declines", out.backend_declines},
        {"backend_fallbacks", out.backend_fallbacks},
        {"stats_saturated", out.stats_saturated},
        {"persisted_gpu_operation_evidence_accepted",
            postgres_sample
                ? nlohmann::json(
                    out.persisted_gpu_operation_evidence_accepted)
                : nlohmann::json{nullptr}},
        {"format5_reader_memory_evidence", postgres_sample
            ? out.format5_reader_memory_evidence
            : nlohmann::json{nullptr}},
        {"format6_reader_evidence", postgres_sample
            ? out.format6_reader_evidence
            : nlohmann::json{nullptr}},
    };
    return payload.dump();
}

// ---------------------------------------------------------------------------
// Cold persisted-index routing POC
// ---------------------------------------------------------------------------
// This benchmark-only model deliberately stops before PostgreSQL/wire changes.
// It uses the real E2E generator and the selected ExactSimd minimizer producer,
// but retains only compact routing metadata plus query hash sets.  Target
// minimizers are collected and discarded one immutable 8-Mbp core at a time.

constexpr std::uint64_t kRoutingPocSegmentBp = 8u * 1024u * 1024u;
constexpr std::uint64_t kRoutingPocMixSeed = 0x6a09e667f3bcc909ULL;
constexpr std::size_t kRoutingPocMaxBottomK = 512u;
constexpr std::uint64_t kRoutingPocScratchBytesPerSegment = 64u;
constexpr std::uint64_t kRoutingPocScratchFixedBytes = 64u * 1024u;
constexpr std::uint64_t kRoutingPocMaterialOverlapPercent = 1u;
constexpr std::uint64_t kRoutingPocDirectoryEntryBytes =
    bioseq::indexing::MinimizerBucketDirEntry::encoded_bytes;
constexpr std::uint64_t kRoutingPocGenerationFixedWireBytes =
    bioseq::indexing::MinimizerGenerationHead::encoded_bytes
    + bioseq::indexing::MinimizerGenerationManifest::encoded_bytes;
constexpr std::uint64_t kRoutingPocPostingWireBytes = 12u;
constexpr std::uint64_t kRoutingPocSignatureDirectoryWireBytes = 16u;
constexpr std::uint64_t kRoutingPocRouterHeaderWireBytes = 64u;
constexpr std::uint64_t kRoutingPocPrefix16OffsetCount = 65537u;
constexpr std::uint64_t kRoutingPocPrefix16OffsetBytes =
    kRoutingPocPrefix16OffsetCount * sizeof(std::uint32_t);
constexpr std::array<std::uint32_t, 4> kRoutingPocSampleRates{
    1024u, 2048u, 4096u, 8192u};
static_assert(kRoutingPocSegmentBp
    <= (std::numeric_limits<std::uint32_t>::max)() / 2u,
    "segment-local position plus strand must fit u32");

[[nodiscard]] constexpr std::uint64_t routing_poc_mix64(
        std::uint64_t value) noexcept {
    value ^= kRoutingPocMixSeed;
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30u)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27u)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31u);
}

[[nodiscard]] constexpr bool routing_poc_power_of_two(
        std::uint32_t value) noexcept {
    return value != 0u && (value & (value - 1u)) == 0u;
}

[[nodiscard]] constexpr std::uint64_t routing_poc_ceil_div(
        std::uint64_t value,
        std::uint64_t divisor) noexcept {
    return value / divisor + (value % divisor != 0u ? 1u : 0u);
}

void routing_poc_checked_add(
        std::uint64_t& destination,
        std::uint64_t increment,
        std::string_view counter) {
    if (increment > (std::numeric_limits<std::uint64_t>::max)()
            - destination) {
        throw std::overflow_error(
            fmt::format("routing POC {} overflow", counter));
    }
    destination += increment;
}

[[nodiscard]] std::uint64_t routing_poc_positive_integer(
        simdjson::dom::element data,
        std::string_view key,
        std::int64_t fallback) {
    const std::int64_t value = jint(data, key, fallback);
    if (value <= 0) {
        throw std::invalid_argument(fmt::format(
            "routing POC '{}' must be a positive integer", key));
    }
    return static_cast<std::uint64_t>(value);
}

[[nodiscard]] std::uint64_t routing_poc_checked_mul(
        std::uint64_t left,
        std::uint64_t right,
        std::string_view counter) {
    if (left != 0u
        && right > (std::numeric_limits<std::uint64_t>::max)() / left) {
        throw std::overflow_error(
            fmt::format("routing POC {} overflow", counter));
    }
    return left * right;
}

[[nodiscard]] std::uint64_t routing_poc_measured_scratch_bytes(
        std::uint64_t segment_count) {
    std::uint64_t bytes = routing_poc_checked_mul(
        segment_count,
        kRoutingPocScratchBytesPerSegment,
        "measured scratch bytes");
    routing_poc_checked_add(
        bytes, kRoutingPocScratchFixedBytes, "measured scratch bytes");
    return bytes;
}

struct RoutingPocPosting {
    std::uint64_t routing_hash = 0;
    std::uint32_t segment = 0;

    [[nodiscard]] auto operator<=>(const RoutingPocPosting&) const = default;
};

struct RoutingPocSampleIndex {
    std::uint32_t rate = 0;
    std::vector<RoutingPocPosting> postings;
};

struct RoutingPocPartition {
    std::uint64_t raw_lo_hash = 0;
    std::uint64_t raw_hi_hash = 0;
    std::uint64_t control_count = 0;
    std::uint64_t position_count = 0;
    std::uint64_t control_bytes = 0;
    std::uint64_t position_bytes = 0;
    std::uint64_t control_chunks = 0;
    std::uint64_t position_chunks = 0;
};

struct RoutingPocSegment {
    std::uint64_t begin_bp = 0;
    std::uint64_t end_bp = 0;
    std::uint64_t position_count = 0;
    std::uint64_t control_count = 0;
    std::uint64_t control_bytes = 0;
    std::uint64_t position_bytes = 0;
    std::uint64_t partition_count = 0;
    std::uint64_t payload_chunks = 0;
    std::vector<RoutingPocPartition> partitions;
    std::vector<std::uint64_t> bottom_k;
};

enum class RoutingPocFixtureKind : std::uint8_t {
    generated,
    boundary_sliver,
    boundary_balanced,
    duplicated_segment,
    tandem_low_complexity,
    short_local_homology,
    global_threshold_repeat,
};

struct RoutingPocFixtureSpec {
    RoutingPocFixtureKind kind = RoutingPocFixtureKind::generated;
    bool expects_fallback = false;
};

struct RoutingPocQuery {
    std::uint64_t truth_begin_bp = 0;
    std::uint64_t truth_end_bp = 0;
    bool reverse = false;
    RoutingPocFixtureKind fixture = RoutingPocFixtureKind::generated;
    bool expects_fallback = false;
    std::uint64_t material_overlap_threshold_bp = 0;
    std::vector<std::uint32_t> primary_truth_segments;
    std::vector<std::uint32_t> material_truth_segments;
    std::vector<std::uint64_t> raw_hashes;
    std::vector<std::uint64_t> routing_hashes;
};

struct RoutingPocInput {
    // Shared so the framework may move its input holder while one reservation
    // continues to cover retained metadata and measured-phase routing scratch.
    std::shared_ptr<bioseq::parallelism::MemoryPermit> retained_permit;
    std::string alphabet;
    std::string selected_backend = "exact_simd_minimap_v1";
    std::uint64_t target_bp = 0;
    std::uint64_t segment_bp = kRoutingPocSegmentBp;
    std::uint64_t admitted_cap_bytes = 0;
    std::uint64_t retained_capacity_bytes = 0;
    std::uint64_t measured_scratch_reserved_bytes = 0;
    // Additional packed target allocation that exists only while a genuinely
    // mutable adversarial fixture owner is cloned from the generated const
    // reference. Random production-scale cases remain zero-copy.
    std::uint64_t fixture_clone_peak_bytes = 0;
    std::uint64_t peak_transient_hash_bytes = 0;
    std::uint64_t total_control_bytes = 0;
    std::uint64_t total_position_bytes = 0;
    std::uint64_t total_payload_chunks = 0;
    std::uint64_t total_payload_rows = 0;
    std::uint64_t total_partitions = 0;
    std::uint64_t directory_bytes = 0;
    std::uint64_t directory_chunks = 0;
    std::uint64_t directory_rows = 0;
    std::uint64_t boundary_query_count = 0;
    std::uint64_t forced_query_count = 0;
    std::uint64_t minimum_truth_overlap_bp = 0;
    bool global_hf_full_scale_blocked = true;
    bool global_hf_fixture_exact = false;
    std::uint64_t global_hf_fixture_hash_count = 0;
    std::uint64_t global_hf_fixture_encoded_bytes = 0;
    std::uint64_t global_hf_fixture_chunks = 0;
    std::array<std::uint64_t, 4> sampled_router_wire_bytes{};
    std::array<std::uint64_t, 4> bottom_k_router_wire_bytes{};
    std::vector<std::uint64_t> global_hf_fixture_hashes;
    bioseq::indexing::ExactSimdIndexExecutionProof simd_proof{};
    std::array<RoutingPocSampleIndex, 4> samples{};
    std::vector<std::uint64_t> batch_raw_hashes;
    std::vector<RoutingPocSegment> segments;
    std::vector<RoutingPocQuery> queries;
};

struct RoutingPocOutput {
    std::uint64_t measured_region_ns = 0;
    std::uint64_t coverage_analysis_ns = 0;
    std::uint64_t query_count = 0;
    std::uint64_t truth_top1_count = 0;
    std::uint64_t truth_top4_count = 0;
    std::uint64_t truth_selected_without_fallback_count = 0;
    std::uint64_t truth_effective_count = 0;
    std::uint64_t truth_rank_sum = 0;
    std::uint64_t truth_rank_max = 0;
    std::uint64_t any_overlap_top1_count = 0;
    std::uint64_t any_overlap_top4_count = 0;
    std::uint64_t any_overlap_selected_without_fallback_count = 0;
    std::uint64_t material_all_selected_without_fallback_count = 0;
    std::uint64_t expected_fallback_queries = 0;
    std::uint64_t expected_fallback_observed = 0;
    std::uint64_t expected_fallback_missed = 0;
    std::uint64_t sampled_query_keys = 0;
    std::uint64_t sampled_query_keys_min = 0;
    std::uint64_t posting_hits = 0;
    std::uint64_t qualified_candidates = 0;
    std::uint64_t fallback_queries = 0;
    std::uint64_t ambiguity_queries = 0;
    std::uint64_t selected_segments = 0;
    std::uint64_t selected_segment_bp = 0;
    std::uint64_t selected_control_bytes = 0;
    std::uint64_t selected_position_bytes = 0;
    std::uint64_t selected_payload_chunks = 0;
    std::uint64_t selected_payload_rows = 0;
    std::uint64_t selected_partitions = 0;
    std::uint64_t baseline_touched_control_partitions = 0;
    std::uint64_t baseline_touched_position_partitions = 0;
    std::uint64_t baseline_touched_control_chunks = 0;
    std::uint64_t baseline_touched_position_chunks = 0;
    std::uint64_t baseline_touched_control_bytes = 0;
    std::uint64_t baseline_touched_position_bytes = 0;
    std::uint64_t routed_touched_control_partitions = 0;
    std::uint64_t routed_touched_position_partitions = 0;
    std::uint64_t routed_touched_control_chunks = 0;
    std::uint64_t routed_touched_position_chunks = 0;
    std::uint64_t routed_touched_control_bytes = 0;
    std::uint64_t routed_touched_position_bytes = 0;
    std::uint64_t boundary_query_count = 0;
    std::uint64_t boundary_truth_selected_count = 0;
    std::uint64_t sample_rate = 0;
    std::uint64_t bottom_k = 0;
    double margin_sum = 0.0;
    double margin_min = (std::numeric_limits<double>::infinity)();
    bool sampled_router = false;
};

struct RoutingPocPartitionAccumulator {
    std::uint64_t controls = 0;
    std::uint64_t positions = 0;
    std::uint64_t raw_lo_hash = 0;
    std::uint64_t raw_hi_hash = 0;
};

void routing_poc_flush_partition(
        RoutingPocPartitionAccumulator& partition,
        RoutingPocSegment& segment) {
    if (partition.controls == 0u) {
        return;
    }
    const std::uint64_t control_bytes = partition.controls * 16u;
    const std::uint64_t position_bytes = partition.positions * 8u;
    const std::uint64_t control_chunks = routing_poc_ceil_div(
        control_bytes, bioseq::indexing::kMinimizerExtentPayloadBytes);
    const std::uint64_t position_chunks = routing_poc_ceil_div(
        position_bytes, bioseq::indexing::kMinimizerExtentPayloadBytes);
    segment.partitions.push_back(RoutingPocPartition{
        .raw_lo_hash = partition.raw_lo_hash,
        .raw_hi_hash = partition.raw_hi_hash,
        .control_count = partition.controls,
        .position_count = partition.positions,
        .control_bytes = control_bytes,
        .position_bytes = position_bytes,
        .control_chunks = control_chunks,
        .position_chunks = position_chunks,
    });
    routing_poc_checked_add(segment.partition_count, 1u, "partition count");
    routing_poc_checked_add(
        segment.payload_chunks,
        control_chunks,
        "control chunk count");
    routing_poc_checked_add(
        segment.payload_chunks,
        position_chunks,
        "position chunk count");
    partition = {};
}

void routing_poc_add_hash_group(
        RoutingPocPartitionAccumulator& partition,
        RoutingPocSegment& segment,
        std::uint64_t raw_hash,
        std::uint64_t position_count) {
    const std::uint64_t group_bytes = 16u + position_count * 8u;
    const std::uint64_t partition_bytes =
        partition.controls * 16u + partition.positions * 8u;
    if (partition.controls != 0u
        && (partition_bytes
                > bioseq::indexing::kMinimizerPartitionTargetBytes
            || group_bytes
                > bioseq::indexing::kMinimizerPartitionTargetBytes
                    - partition_bytes)) {
        routing_poc_flush_partition(partition, segment);
    }
    if (partition.controls == 0u) {
        partition.raw_lo_hash = raw_hash;
    }
    partition.raw_hi_hash = raw_hash;
    ++partition.controls;
    routing_poc_checked_add(
        partition.positions, position_count, "partition positions");
}

template <class Sequence>
[[nodiscard]] std::vector<std::uint64_t> routing_poc_collect_core_hashes(
        Sequence& sequence,
        std::uint64_t core_begin_bp,
        std::uint64_t core_end_bp,
        std::uint16_t window_size,
        std::uint16_t kmer_size,
        std::uint64_t& peak_transient_bytes,
        bioseq::indexing::ExactSimdIndexExecutionProof& aggregate_proof) {
    using sequence_type = std::remove_cvref_t<Sequence>;
    using traits = typename sequence_type::traits;
    constexpr std::uint64_t letters_per_unit = traits::letters_per_unit;
    const std::uint64_t target_bp = sequence.bp_size();
    const std::uint64_t halo = static_cast<std::uint64_t>(window_size)
        + kmer_size - 2u;
    const std::uint64_t collect_begin = core_begin_bp >= halo
        ? core_begin_bp - halo : 0u;
    const std::uint64_t collect_end = std::min<std::uint64_t>(
        target_bp,
        core_end_bp > target_bp - std::min(target_bp, halo)
            ? target_bp : core_end_bp + halo);
    const std::uint64_t begin_unit = collect_begin / letters_per_unit;
    const std::uint64_t end_unit = collect_end / letters_per_unit
        + (collect_end % letters_per_unit != 0u ? 1u : 0u);
    const std::uint64_t emitted_base = begin_unit * letters_per_unit;

    bioseq::indexing::sketch_collector collector;
    const auto proof = bioseq::indexing::collect_exact_simd_index_range_or_throw(
        collector,
        sequence,
        static_cast<std::size_t>(begin_unit),
        static_cast<std::size_t>(end_unit),
        window_size,
        kmer_size,
        0u,
        "routing POC target segment");
    if (!proof.accepted(/*require_nonempty_vector_work=*/true)) {
        throw std::runtime_error(
            "routing POC ExactSimd target sketch proof was not accepted");
    }
    aggregate_proof.combine(proof);

    const auto& sketches = collector.extract_sketches_data();
    std::vector<std::uint64_t> hashes;
    hashes.reserve(sketches.size());
    for (const bioseq::indexing::MinimapSketchData& sketch : sketches) {
        const std::uint64_t position = emitted_base
            + static_cast<std::uint64_t>(sketch.last_pos());
        if (position >= core_begin_bp && position < core_end_bp) {
            hashes.push_back(sketch.kmer());
        }
    }
    std::uint64_t concurrent_capacity = routing_poc_checked_mul(
        sketches.capacity(),
        sizeof(bioseq::indexing::MinimapSketchData),
        "target collector capacity bytes");
    routing_poc_checked_add(
        concurrent_capacity,
        routing_poc_checked_mul(
            hashes.capacity(), sizeof(std::uint64_t),
            "target hash capacity bytes"),
        "target transient capacity bytes");
    peak_transient_bytes = std::max(
        peak_transient_bytes, concurrent_capacity);
    return hashes;
}

template <class Sequence>
[[nodiscard]] std::vector<std::uint64_t> routing_poc_collect_query_hashes(
        Sequence& sequence,
        std::uint16_t window_size,
        std::uint16_t kmer_size,
        std::uint64_t& peak_transient_bytes,
        bioseq::indexing::ExactSimdIndexExecutionProof& aggregate_proof) {
    bioseq::indexing::sketch_collector collector;
    const auto proof = bioseq::indexing::collect_exact_simd_index_range_or_throw(
        collector,
        sequence,
        0u,
        static_cast<std::size_t>(sequence.size()),
        window_size,
        kmer_size,
        0u,
        "routing POC query");
    if (!proof.accepted(/*require_nonempty_vector_work=*/true)) {
        throw std::runtime_error(
            "routing POC ExactSimd query sketch proof was not accepted");
    }
    aggregate_proof.combine(proof);
    const auto& sketches = collector.extract_sketches_data();
    std::vector<std::uint64_t> hashes;
    hashes.reserve(sketches.size());
    for (const bioseq::indexing::MinimapSketchData& sketch : sketches) {
        hashes.push_back(sketch.kmer());
    }
    std::uint64_t concurrent_capacity = routing_poc_checked_mul(
        sketches.capacity(),
        sizeof(bioseq::indexing::MinimapSketchData),
        "query collector capacity bytes");
    routing_poc_checked_add(
        concurrent_capacity,
        routing_poc_checked_mul(
            hashes.capacity(), sizeof(std::uint64_t),
            "query hash capacity bytes"),
        "query transient capacity bytes");
    peak_transient_bytes = std::max(
        peak_transient_bytes, concurrent_capacity);
    std::ranges::sort(hashes);
    hashes.erase(std::unique(hashes.begin(), hashes.end()), hashes.end());
    return hashes;
}

[[nodiscard]] std::uint64_t routing_poc_retained_capacity_bytes(
        const RoutingPocInput& input) {
    std::uint64_t bytes = 0;
    routing_poc_checked_add(
        bytes,
        input.segments.capacity() * sizeof(RoutingPocSegment),
        "retained segment bytes");
    for (const RoutingPocSegment& segment : input.segments) {
        routing_poc_checked_add(
            bytes,
            segment.partitions.capacity() * sizeof(RoutingPocPartition),
            "retained partition-directory bytes");
        routing_poc_checked_add(
            bytes,
            segment.bottom_k.capacity() * sizeof(std::uint64_t),
            "retained segment signature bytes");
    }
    routing_poc_checked_add(
        bytes,
        input.queries.capacity() * sizeof(RoutingPocQuery),
        "retained query bytes");
    for (const RoutingPocQuery& query : input.queries) {
        routing_poc_checked_add(
            bytes,
            query.primary_truth_segments.capacity()
                * sizeof(std::uint32_t),
            "retained primary-truth bytes");
        routing_poc_checked_add(
            bytes,
            query.material_truth_segments.capacity()
                * sizeof(std::uint32_t),
            "retained material-truth bytes");
        routing_poc_checked_add(
            bytes,
            query.raw_hashes.capacity() * sizeof(std::uint64_t),
            "retained query raw-hash bytes");
        routing_poc_checked_add(
            bytes,
            query.routing_hashes.capacity() * sizeof(std::uint64_t),
            "retained query hash bytes");
    }
    routing_poc_checked_add(
        bytes,
        input.batch_raw_hashes.capacity() * sizeof(std::uint64_t),
        "retained batch raw-hash union bytes");
    routing_poc_checked_add(
        bytes,
        input.global_hf_fixture_hashes.capacity() * sizeof(std::uint64_t),
        "retained global-HF fixture hash bytes");
    for (const RoutingPocSampleIndex& sample : input.samples) {
        routing_poc_checked_add(
            bytes,
            sample.postings.capacity() * sizeof(RoutingPocPosting),
            "retained posting bytes");
    }
    return bytes;
}

template <sequence_alphabet A>
[[nodiscard]] RunConstraints routing_poc_estimate_typed(
        simdjson::dom::element data) {
    using traits = typename bench_sequence_range<A>::traits;
    const std::uint64_t target_bp = routing_poc_positive_integer(
        data, "reference_length", kDefaultReferenceLengthBp);
    const std::uint64_t query_count = routing_poc_positive_integer(
        data, "fragment_count", 16);
    const std::uint64_t query_bp = routing_poc_positive_integer(
        data, "fragment_length_mean", 5'000'000);
    const std::uint64_t target_units = routing_poc_ceil_div(
        target_bp, traits::letters_per_unit);
    const std::uint64_t query_units = routing_poc_ceil_div(
        query_bp, traits::letters_per_unit);
    const std::uint64_t packed_target = routing_poc_checked_mul(
        target_units, sizeof(bench_storage_t), "packed target bytes");
    const std::uint64_t packed_query = routing_poc_checked_mul(
        query_units, sizeof(bench_storage_t), "packed query bytes");
    const std::uint64_t packed_queries = routing_poc_checked_mul(
        query_count, packed_query, "packed query aggregate bytes");
    const std::uint64_t cap_mb = routing_poc_positive_integer(
        data, "routing_memory_cap_mb", 512);
    const std::uint64_t cap_bytes = routing_poc_checked_mul(
        cap_mb, 1024u * 1024u, "admission cap bytes");
    std::uint64_t base_memory = packed_target;
    routing_poc_checked_add(
        base_memory, packed_queries, "generator base-memory estimate");
    const std::string corpus_kind = jstr(
        data, "routing_corpus_kind", "random");
    if (corpus_kind != "random") {
        std::uint64_t clone_peak_bytes = packed_target;
        routing_poc_checked_add(
            clone_peak_bytes,
            sizeof(bench_sequence_range<A>),
            "adversarial mutable target clone bytes");
        routing_poc_checked_add(
            base_memory,
            clone_peak_bytes,
            "adversarial fixture generation peak");
    }
    if (base_memory
            > static_cast<std::uint64_t>(
                (std::numeric_limits<std::size_t>::max)())
        || cap_bytes
            > static_cast<std::uint64_t>(
                (std::numeric_limits<std::size_t>::max)())) {
        throw std::overflow_error(
            "routing POC resource estimate exceeds size_t");
    }
    return RunConstraints{
        .base_memory = static_cast<std::size_t>(base_memory),
        .extra_memory = static_cast<std::size_t>(cap_bytes),
    };
}

[[nodiscard]] RunConstraints routing_poc_estimate(
        std::string_view,
        std::string_view,
        simdjson::dom::element data) {
    RunConstraints result{};
    dispatch_bench_alphabet(jstr(data, "alphabet", "dna4"),
        [&result, data]<sequence_alphabet A>() {
            result = routing_poc_estimate_typed<A>(data);
        });
    return result;
}

struct RoutingPocForcedQueryPlan {
    std::uint64_t begin_bp = 0;
    std::uint64_t end_bp = 0;
    RoutingPocFixtureKind kind = RoutingPocFixtureKind::generated;
    bool expects_fallback = false;
};

template <sequence_alphabet A>
void routing_poc_mutate_fixture_target(
        bench_sequence_range<A>& target,
        std::string_view corpus_kind) {
    if (corpus_kind == "random") {
        return;
    }
    if constexpr (!std::same_as<A, seq::dna4>) {
        throw std::invalid_argument(
            "routing POC adversarial corpora are currently dna4-only");
    } else {
        using sequence_type = bench_sequence_range<A>;
        constexpr std::uint64_t lpu = sequence_type::traits::letters_per_unit;
        if (kRoutingPocSegmentBp % lpu != 0u) {
            throw std::runtime_error(
                "routing POC fixture requires unit-aligned 8-Mbp segments");
        }
        const std::uint64_t units_per_segment = kRoutingPocSegmentBp / lpu;
        const std::uint64_t segment_count = routing_poc_ceil_div(
            target.bp_size(), kRoutingPocSegmentBp);
        auto require_segments = [segment_count](std::uint64_t required) {
            if (segment_count < required) {
                throw std::invalid_argument(fmt::format(
                    "routing POC corpus requires at least {} segments",
                    required));
            }
        };
        if (corpus_kind == "duplicated_segment") {
            require_segments(6u);
            for (std::uint64_t destination = 1u;
                 destination < 6u; ++destination) {
                std::copy_n(
                    target._data.begin(),
                    static_cast<std::ptrdiff_t>(units_per_segment),
                    target._data.begin() + static_cast<std::ptrdiff_t>(
                        destination * units_per_segment));
            }
        } else if (corpus_kind == "tandem_low_complexity") {
            require_segments(6u);
            std::fill_n(
                target._data.begin(),
                static_cast<std::ptrdiff_t>(6u * units_per_segment),
                typename sequence_type::traits::storage_unit{0});
        } else if (corpus_kind == "short_local_homology") {
            require_segments(6u);
            constexpr std::uint64_t local_bp = 256u * 1024u;
            constexpr std::uint64_t local_offset_bp = 1024u * 1024u;
            const std::uint64_t local_units = local_bp / lpu;
            const std::uint64_t local_offset_units = local_offset_bp / lpu;
            for (std::uint64_t destination = 1u;
                 destination < 6u; ++destination) {
                std::copy_n(
                    target._data.begin()
                        + static_cast<std::ptrdiff_t>(local_offset_units),
                    static_cast<std::ptrdiff_t>(local_units),
                    target._data.begin() + static_cast<std::ptrdiff_t>(
                        destination * units_per_segment
                        + local_offset_units));
            }
        } else if (corpus_kind == "global_threshold_repeat") {
            require_segments(2u);
            // Fifteen packed dna4 words = 480 bp. A homopolymer minimizer
            // occurs fewer than 500 times per segment but more than 500 times
            // across two copies; the streamed counters below verify rather
            // than assume those inequalities.
            constexpr std::uint64_t motif_bp = 480u;
            constexpr std::uint64_t motif_offset_bp = 1024u * 1024u;
            const std::uint64_t motif_units = motif_bp / lpu;
            const std::uint64_t motif_offset_units = motif_offset_bp / lpu;
            for (std::uint64_t segment = 0u; segment < 2u; ++segment) {
                std::fill_n(
                    target._data.begin() + static_cast<std::ptrdiff_t>(
                        segment * units_per_segment + motif_offset_units),
                    static_cast<std::ptrdiff_t>(motif_units),
                    typename sequence_type::traits::storage_unit{0});
            }
        } else {
            throw std::invalid_argument(fmt::format(
                "unknown routing POC corpus_kind '{}'", corpus_kind));
        }
    }
}

template <sequence_alphabet A>
[[nodiscard]] bench_pair_t<A> routing_poc_make_exact_query(
        const std::shared_ptr<const bench_sequence_range<A>>& target,
        const RoutingPocForcedQueryPlan& plan) {
    using sequence_type = bench_sequence_range<A>;
    using rank_type = typename sequence_type::traits::letter_unit;
    if (target == nullptr || plan.begin_bp >= plan.end_bp
        || plan.end_bp > target->bp_size()) {
        throw std::invalid_argument(
            "routing POC forced-query interval is invalid");
    }
    std::vector<rank_type> ranks(plan.end_bp - plan.begin_bp);
    gen::extract_window_ranks(
        *target,
        static_cast<std::size_t>(plan.begin_bp),
        static_cast<std::size_t>(plan.end_bp),
        false,
        std::span<rank_type>{ranks.data(), ranks.size()});
    std::vector<A> letters(ranks.size());
    for (std::size_t index = 0; index < ranks.size(); ++index) {
        letters[index].assign_rank(ranks[index]);
    }
    bench_pair_t<A> pair;
    pair.query = sequence_type{std::move(letters)};
    pair.target = target;
    pair.truth_target_start = static_cast<std::size_t>(plan.begin_bp);
    pair.truth_target_end = static_cast<std::size_t>(plan.end_bp);
    pair.is_reverse = false;
    pair.effective_error_rate = 0.0;
    pair.original_query_length = static_cast<std::size_t>(
        plan.end_bp - plan.begin_bp);
    return pair;
}

[[nodiscard]] std::vector<RoutingPocForcedQueryPlan>
routing_poc_forced_query_plans(
        std::uint64_t reference_length,
        std::uint64_t normal_query_bp,
        std::string_view corpus_kind,
        bool add_boundary_fixtures) {
    std::vector<RoutingPocForcedQueryPlan> plans;
    const std::uint64_t boundary = kRoutingPocSegmentBp;
    if (add_boundary_fixtures && reference_length > boundary
        && normal_query_bp < reference_length
        && normal_query_bp / 2u < boundary) {
        const std::uint64_t balanced_begin = boundary - normal_query_bp / 2u;
        plans.push_back(RoutingPocForcedQueryPlan{
            .begin_bp = balanced_begin,
            .end_bp = balanced_begin + normal_query_bp,
            .kind = RoutingPocFixtureKind::boundary_balanced,
        });
        const std::uint64_t material_threshold = std::max<std::uint64_t>(
            15u, routing_poc_ceil_div(normal_query_bp, 100u));
        const std::uint64_t sliver = std::max<std::uint64_t>(
            1u, material_threshold / 2u);
        plans.push_back(RoutingPocForcedQueryPlan{
            .begin_bp = boundary - (normal_query_bp - sliver),
            .end_bp = boundary + sliver,
            .kind = RoutingPocFixtureKind::boundary_sliver,
        });
    }
    if (corpus_kind == "duplicated_segment") {
        plans.push_back(RoutingPocForcedQueryPlan{
            .begin_bp = 1024u * 1024u,
            .end_bp = 1024u * 1024u + normal_query_bp,
            .kind = RoutingPocFixtureKind::duplicated_segment,
            .expects_fallback = true,
        });
    } else if (corpus_kind == "tandem_low_complexity") {
        plans.push_back(RoutingPocForcedQueryPlan{
            .begin_bp = 1024u * 1024u,
            .end_bp = 1024u * 1024u + normal_query_bp,
            .kind = RoutingPocFixtureKind::tandem_low_complexity,
            .expects_fallback = true,
        });
    } else if (corpus_kind == "short_local_homology") {
        plans.push_back(RoutingPocForcedQueryPlan{
            .begin_bp = 1024u * 1024u,
            .end_bp = 1024u * 1024u + 256u * 1024u,
            .kind = RoutingPocFixtureKind::short_local_homology,
            .expects_fallback = true,
        });
    } else if (corpus_kind == "global_threshold_repeat") {
        plans.push_back(RoutingPocForcedQueryPlan{
            .begin_bp = 1024u * 1024u,
            .end_bp = 1024u * 1024u + 480u,
            .kind = RoutingPocFixtureKind::global_threshold_repeat,
            .expects_fallback = true,
        });
    }
    for (const RoutingPocForcedQueryPlan& plan : plans) {
        if (plan.end_bp > reference_length) {
            throw std::invalid_argument(
                "routing POC forced fixture exceeds reference length");
        }
    }
    return plans;
}

template <sequence_alphabet A>
[[nodiscard]] RoutingPocInput routing_poc_input_typed(
        std::string_view test_case_name,
        simdjson::dom::element data) {
    const std::string backend = jstr(
        data, "sketch_backend", "exact_simd_minimap_v1");
    if (backend != "exact_simd_minimap_v1") {
        throw std::invalid_argument(fmt::format(
            "routing POC case '{}' requested unsupported sketch backend '{}'",
            test_case_name,
            backend));
    }

    const std::uint64_t reference_length = routing_poc_positive_integer(
        data, "reference_length", kDefaultReferenceLengthBp);
    const std::uint64_t fragment_count_u64 = routing_poc_positive_integer(
        data, "fragment_count", 16);
    const std::uint64_t window_size_u64 = routing_poc_positive_integer(
        data, "window_size", kDefaultWindowSize);
    const std::uint64_t kmer_size_u64 = routing_poc_positive_integer(
        data, "kmer_size", kDefaultKmerSize);
    const std::uint64_t cap_mb = routing_poc_positive_integer(
        data, "routing_memory_cap_mb", 512);
    if (reference_length
            > static_cast<std::uint64_t>(
                (std::numeric_limits<std::size_t>::max)())
        || fragment_count_u64
            > static_cast<std::uint64_t>(
                (std::numeric_limits<std::size_t>::max)())
        || window_size_u64
            > (std::numeric_limits<std::uint16_t>::max)()
        || kmer_size_u64
            > (std::numeric_limits<std::uint16_t>::max)()) {
        throw std::invalid_argument(
            "routing POC generation/admission parameters are invalid");
    }
    // This POC proves only the selected Exact minimap-compatible W10/K15
    // producer. It is not evidence for the separately qualifying Paper ntHash
    // route, whose hash population and sampling density must be measured in a
    // dedicated follow-up.
    if (window_size_u64 != 10u || kmer_size_u64 != 15u) {
        throw std::invalid_argument(
            "routing POC Exact minimap scheme requires window_size=10 and kmer_size=15");
    }
    const auto fragment_count = static_cast<std::size_t>(fragment_count_u64);
    const auto window_size = static_cast<std::uint16_t>(window_size_u64);
    const auto kmer_size = static_cast<std::uint16_t>(kmer_size_u64);
    const std::uint64_t cap_bytes = routing_poc_checked_mul(
        cap_mb, 1024u * 1024u, "admission cap bytes");
    auto permit = bioseq::parallelism::MemoryAdmissionController::instance()
        .try_reserve(cap_bytes);
    if (!permit) {
        throw std::runtime_error(fmt::format(
            "routing POC could not reserve {} MiB", cap_mb));
    }

    RoutingPocInput input;
    input.retained_permit =
        std::make_shared<bioseq::parallelism::MemoryPermit>(std::move(*permit));
    input.alphabet = std::string{bench_alphabet_name<A>()};
    input.selected_backend = backend;
    input.target_bp = reference_length;
    input.admitted_cap_bytes = cap_bytes;
    for (std::size_t index = 0; index < input.samples.size(); ++index) {
        input.samples[index].rate = kRoutingPocSampleRates[index];
    }

    gen::generator_params params{};
    params.reference_length = static_cast<std::size_t>(reference_length);
    params.fragment_count = fragment_count;
    const std::uint64_t fragment_length_mean = routing_poc_positive_integer(
        data, "fragment_length_mean", 5'000'000);
    if (fragment_length_mean
        > static_cast<std::uint64_t>(
            (std::numeric_limits<std::size_t>::max)())) {
        throw std::invalid_argument(
            "routing POC fragment_length_mean exceeds size_t");
    }
    params.fragment_length_mean =
        static_cast<std::size_t>(fragment_length_mean);
    params.fragment_length_stddev =
        jdbl(data, "fragment_length_stddev", 100'000.0);
    params.substitution_rate = jdbl(data, "substitution_rate", 0.001);
    params.insertion_rate = jdbl(data, "insertion_rate", 0.0005);
    params.deletion_rate = jdbl(data, "deletion_rate", 0.0005);
    params.insertion_length_mean =
        jdbl(data, "insertion_length_mean", 1.0);
    params.deletion_length_mean =
        jdbl(data, "deletion_length_mean", 1.0);
    params.reverse_complement_rate =
        jdbl(data, "reverse_complement_rate", 0.5);
    params.seed = static_cast<std::uint64_t>(
        jint(data, "seed", 3'219'876));
    params.use_vulkan = false;

    const std::string corpus_kind = jstr(data, "routing_corpus_kind", "random");
    const bool add_boundary_fixtures =
        jint(data, "routing_add_boundary_fixtures", 1) != 0;
    auto generator = gen::alphabet_sequence_generator<A, bench_storage_t>{params};
    auto reference = generator.generate_reference();
    if (reference == nullptr || reference->bp_size() != reference_length) {
        throw std::runtime_error(
            "routing POC generator did not return the requested reference");
    }
    using sequence_type = bench_sequence_range<A>;
    if (corpus_kind != "random") {
        // generate_reference() deliberately returns an actually const object.
        // Build a genuinely mutable owner for the bounded adversarial fixtures,
        // mutate it, and only then expose it through the generator's const
        // target interface. The original allocation is released by assignment;
        // its brief overlap with the clone is charged in RunConstraints above.
        auto mutable_reference = std::make_shared<sequence_type>(*reference);
        input.fixture_clone_peak_bytes = routing_poc_checked_mul(
            mutable_reference->_data.capacity(),
            sizeof(typename sequence_type::traits::storage_unit),
            "adversarial mutable target payload bytes");
        routing_poc_checked_add(
            input.fixture_clone_peak_bytes,
            sizeof(sequence_type),
            "adversarial mutable target clone bytes");
        routing_poc_mutate_fixture_target(*mutable_reference, corpus_kind);
        reference = std::move(mutable_reference);
    }
    // The Exact-SIMD erased bridge currently takes a non-const reference so it
    // can also drive mutable fragmented scanners. This POC's resident target
    // use is strictly read-only (size/read_cptr only); removing const here does
    // not modify the actually-const random reference. Adversarial mutation has
    // already completed through its genuinely mutable owner above.
    sequence_type& target_read_view =
        const_cast<sequence_type&>(*reference);
    auto pairs = generator.generate_fragment_pairs(
        reference, 0u, fragment_count);
    if (pairs.size() != fragment_count || pairs.empty()
        || pairs.front().target == nullptr
        || pairs.front().target->bp_size() != reference_length) {
        throw std::runtime_error(
            "routing POC generator did not return the requested target/query shape");
    }

    const std::uint64_t segment_count = routing_poc_ceil_div(
        reference_length, kRoutingPocSegmentBp);
    if (segment_count
        > static_cast<std::uint64_t>(
            (std::numeric_limits<std::uint32_t>::max)())) {
        throw std::overflow_error("routing POC segment count exceeds uint32");
    }
    input.measured_scratch_reserved_bytes =
        routing_poc_measured_scratch_bytes(segment_count);
    input.segments.reserve(static_cast<std::size_t>(segment_count));

    std::vector<RoutingPocFixtureSpec> fixture_specs(
        pairs.size(), RoutingPocFixtureSpec{});
    const auto forced_plans = routing_poc_forced_query_plans(
        reference_length,
        fragment_length_mean,
        corpus_kind,
        add_boundary_fixtures);
    for (const RoutingPocForcedQueryPlan& plan : forced_plans) {
        pairs.push_back(routing_poc_make_exact_query(reference, plan));
        fixture_specs.push_back(RoutingPocFixtureSpec{
            .kind = plan.kind,
            .expects_fallback = plan.expects_fallback,
        });
    }
    input.forced_query_count = forced_plans.size();

    input.queries.reserve(pairs.size());
    std::uint64_t raw_union_reserve = 0;
    for (std::size_t pair_index = 0; pair_index < pairs.size(); ++pair_index) {
        bench_pair_t<A>& pair = pairs[pair_index];
        RoutingPocQuery query{
            .truth_begin_bp = pair.truth_target_start,
            .truth_end_bp = pair.truth_target_end,
            .reverse = pair.is_reverse,
            .fixture = fixture_specs[pair_index].kind,
            .expects_fallback = fixture_specs[pair_index].expects_fallback,
            .raw_hashes = routing_poc_collect_query_hashes(
                pair.query,
                window_size,
                kmer_size,
                input.peak_transient_hash_bytes,
                input.simd_proof),
        };
        if (query.truth_begin_bp >= query.truth_end_bp
            || query.truth_end_bp > reference_length
            || query.raw_hashes.empty()) {
            throw std::runtime_error(
                "routing POC query truth/hash set is malformed");
        }
        query.routing_hashes.reserve(query.raw_hashes.size());
        for (std::uint64_t raw_hash : query.raw_hashes) {
            query.routing_hashes.push_back(routing_poc_mix64(raw_hash));
        }
        std::ranges::sort(query.routing_hashes);

        const std::uint64_t truth_span =
            query.truth_end_bp - query.truth_begin_bp;
        // "Material" deliberately excludes boundary dust: a segment must own
        // at least max(k, ceil(1% of the source truth span)). Primary truth is
        // the segment (or tied segments) with maximum base-pair overlap.
        query.material_overlap_threshold_bp = std::max<std::uint64_t>(
            kmer_size,
            routing_poc_ceil_div(
                truth_span * kRoutingPocMaterialOverlapPercent, 100u));
        const std::uint64_t first_segment =
            query.truth_begin_bp / kRoutingPocSegmentBp;
        const std::uint64_t last_segment =
            (query.truth_end_bp - 1u) / kRoutingPocSegmentBp;
        if (first_segment != last_segment) {
            ++input.boundary_query_count;
        }
        std::vector<std::pair<std::uint32_t, std::uint64_t>> overlaps;
        std::uint64_t maximum_overlap = 0;
        for (std::uint64_t segment = first_segment;
             segment <= last_segment; ++segment) {
            const std::uint64_t segment_begin =
                segment * kRoutingPocSegmentBp;
            const std::uint64_t segment_end = std::min<std::uint64_t>(
                reference_length,
                segment_begin + std::min(
                    kRoutingPocSegmentBp,
                    reference_length - segment_begin));
            const std::uint64_t overlap =
                std::min(query.truth_end_bp, segment_end)
                - std::max(query.truth_begin_bp, segment_begin);
            overlaps.emplace_back(
                static_cast<std::uint32_t>(segment), overlap);
            maximum_overlap = std::max(maximum_overlap, overlap);
            input.minimum_truth_overlap_bp =
                input.minimum_truth_overlap_bp == 0u
                ? overlap
                : std::min(input.minimum_truth_overlap_bp, overlap);
        }
        for (const auto& [segment, overlap] : overlaps) {
            if (overlap == maximum_overlap) {
                query.primary_truth_segments.push_back(segment);
            }
            if (overlap >= query.material_overlap_threshold_bp) {
                query.material_truth_segments.push_back(segment);
            }
        }
        if (query.primary_truth_segments.empty()
            || query.material_truth_segments.empty()) {
            throw std::runtime_error(
                "routing POC query has no primary/material truth segment");
        }
        routing_poc_checked_add(
            raw_union_reserve,
            query.raw_hashes.size(),
            "batch raw-hash union reserve");
        input.queries.push_back(std::move(query));
    }
    if (raw_union_reserve
        > static_cast<std::uint64_t>(
            (std::numeric_limits<std::size_t>::max)())) {
        throw std::overflow_error(
            "routing POC batch raw-hash union exceeds size_t");
    }
    input.batch_raw_hashes.reserve(
        static_cast<std::size_t>(raw_union_reserve));
    for (const RoutingPocQuery& query : input.queries) {
        input.batch_raw_hashes.insert(
            input.batch_raw_hashes.end(),
            query.raw_hashes.begin(),
            query.raw_hashes.end());
    }
    std::ranges::sort(input.batch_raw_hashes);
    input.batch_raw_hashes.erase(
        std::unique(
            input.batch_raw_hashes.begin(), input.batch_raw_hashes.end()),
        input.batch_raw_hashes.end());

    const auto global_fixture = std::find_if(
        input.queries.begin(), input.queries.end(),
        [](const RoutingPocQuery& query) {
            return query.fixture
                == RoutingPocFixtureKind::global_threshold_repeat;
        });
    std::vector<std::uint64_t> global_fixture_counts(
        global_fixture == input.queries.end()
            ? 0u : global_fixture->raw_hashes.size(), 0u);
    std::vector<std::uint64_t> global_fixture_segment_max(
        global_fixture_counts.size(), 0u);

    for (std::uint64_t ordinal = 0; ordinal < segment_count; ++ordinal) {
        const std::uint64_t begin_bp = ordinal * kRoutingPocSegmentBp;
        const std::uint64_t end_bp = std::min<std::uint64_t>(
            reference_length,
            begin_bp + std::min(
                kRoutingPocSegmentBp, reference_length - begin_bp));
        std::vector<std::uint64_t> hashes = routing_poc_collect_core_hashes(
            target_read_view,
            begin_bp,
            end_bp,
            window_size,
            kmer_size,
            input.peak_transient_hash_bytes,
            input.simd_proof);
        std::ranges::sort(hashes);

        if (global_fixture != input.queries.end()) {
            for (std::size_t fixture_hash = 0;
                 fixture_hash < global_fixture->raw_hashes.size();
                 ++fixture_hash) {
                const auto range = std::equal_range(
                    hashes.begin(),
                    hashes.end(),
                    global_fixture->raw_hashes[fixture_hash]);
                const std::uint64_t occurrences = static_cast<std::uint64_t>(
                    range.second - range.first);
                routing_poc_checked_add(
                    global_fixture_counts[fixture_hash],
                    occurrences,
                    "global repeat fixture count");
                global_fixture_segment_max[fixture_hash] = std::max(
                    global_fixture_segment_max[fixture_hash], occurrences);
            }
        }

        RoutingPocSegment segment{
            .begin_bp = begin_bp,
            .end_bp = end_bp,
            .position_count = hashes.size(),
        };
        RoutingPocPartitionAccumulator partition{};
        std::size_t cursor = 0;
        std::size_t unique_count = 0;
        while (cursor < hashes.size()) {
            const std::uint64_t raw_hash = hashes[cursor];
            std::size_t end = cursor + 1u;
            while (end < hashes.size() && hashes[end] == raw_hash) {
                ++end;
            }
            const std::uint64_t count = end - cursor;
            const std::uint64_t routing_hash = routing_poc_mix64(raw_hash);
            for (RoutingPocSampleIndex& sample : input.samples) {
                if ((routing_hash & (sample.rate - 1u)) == 0u) {
                    sample.postings.push_back(RoutingPocPosting{
                        .routing_hash = routing_hash,
                        .segment = static_cast<std::uint32_t>(ordinal),
                    });
                }
            }
            routing_poc_add_hash_group(
                partition, segment, raw_hash, count);
            // Safe in-place compaction: unique_count never exceeds cursor, so
            // writing the mixed value cannot overwrite an unread run.
            hashes[unique_count++] = routing_hash;
            cursor = end;
        }
        routing_poc_flush_partition(partition, segment);
        segment.control_count = unique_count;
        segment.control_bytes = segment.control_count * 16u;
        segment.position_bytes = segment.position_count * 8u;
        hashes.resize(unique_count);
        if (hashes.size() > kRoutingPocMaxBottomK) {
            std::nth_element(
                hashes.begin(),
                hashes.begin()
                    + static_cast<std::ptrdiff_t>(kRoutingPocMaxBottomK),
                hashes.end());
            hashes.resize(kRoutingPocMaxBottomK);
        }
        std::ranges::sort(hashes);
        // Do not move the segment-sized working vector: resize() would retain
        // its multi-megabyte capacity in every 512-entry signature. Copy the
        // bounded prefix into a right-sized owner and release segment scratch.
        segment.bottom_k.assign(hashes.begin(), hashes.end());
        segment.bottom_k.shrink_to_fit();

        routing_poc_checked_add(
            input.total_control_bytes,
            segment.control_bytes,
            "total control bytes");
        routing_poc_checked_add(
            input.total_position_bytes,
            segment.position_bytes,
            "total position bytes");
        routing_poc_checked_add(
            input.total_payload_chunks,
            segment.payload_chunks,
            "total payload chunks");
        routing_poc_checked_add(
            input.total_partitions,
            segment.partition_count,
            "total partitions");
        input.segments.push_back(std::move(segment));
    }
    input.total_payload_rows = input.total_payload_chunks;

    for (RoutingPocSampleIndex& sample : input.samples) {
        std::ranges::sort(sample.postings);
        const auto duplicate = std::adjacent_find(
            sample.postings.begin(), sample.postings.end(),
            [](const RoutingPocPosting& left,
               const RoutingPocPosting& right) noexcept {
                return left.routing_hash == right.routing_hash
                    && left.segment == right.segment;
            });
        if (duplicate != sample.postings.end()) {
            throw std::runtime_error(
                "routing POC produced a duplicate (hash,segment) posting");
        }
    }

    input.directory_bytes = routing_poc_checked_mul(
        input.total_partitions,
        kRoutingPocDirectoryEntryBytes,
        "directory wire bytes");
    input.directory_chunks = routing_poc_ceil_div(
        input.directory_bytes,
        bioseq::indexing::kMinimizerExtentPayloadBytes);
    input.directory_rows = input.directory_chunks;

    for (std::size_t sample_index = 0;
         sample_index < input.samples.size(); ++sample_index) {
        std::uint64_t wire_bytes = kRoutingPocRouterHeaderWireBytes;
        routing_poc_checked_add(
            wire_bytes,
            routing_poc_checked_mul(
                input.segments.size(),
                kRoutingPocSignatureDirectoryWireBytes,
                "sample-router directory bytes"),
            "sample-router wire bytes");
        routing_poc_checked_add(
            wire_bytes,
            routing_poc_checked_mul(
                input.samples[sample_index].postings.size(),
                kRoutingPocPostingWireBytes,
                "sample-router posting bytes"),
            "sample-router wire bytes");
        input.sampled_router_wire_bytes[sample_index] = wire_bytes;
    }
    constexpr std::array<std::size_t, 4> bottom_k_values{
        64u, 128u, 256u, 512u};
    for (std::size_t k_index = 0; k_index < bottom_k_values.size(); ++k_index) {
        std::uint64_t wire_bytes = kRoutingPocRouterHeaderWireBytes;
        routing_poc_checked_add(
            wire_bytes,
            routing_poc_checked_mul(
                input.segments.size(),
                kRoutingPocSignatureDirectoryWireBytes,
                "bottom-K directory bytes"),
            "bottom-K wire bytes");
        for (const RoutingPocSegment& segment : input.segments) {
            routing_poc_checked_add(
                wire_bytes,
                routing_poc_checked_mul(
                    std::min(bottom_k_values[k_index], segment.bottom_k.size()),
                    sizeof(std::uint64_t),
                    "bottom-K signature bytes"),
                "bottom-K wire bytes");
        }
        input.bottom_k_router_wire_bytes[k_index] = wire_bytes;
    }

    if (global_fixture != input.queries.end()) {
        bool validated_cross_segment_threshold = false;
        for (std::size_t index = 0;
            index < global_fixture_counts.size(); ++index) {
            if (global_fixture_counts[index] > 500u) {
                input.global_hf_fixture_hashes.push_back(
                    global_fixture->raw_hashes[index]);
                validated_cross_segment_threshold =
                    validated_cross_segment_threshold
                    || global_fixture_segment_max[index] <= 500u;
            }
        }
        input.global_hf_fixture_hash_count =
            input.global_hf_fixture_hashes.size();
        if (!validated_cross_segment_threshold) {
            throw std::runtime_error(
                "routing POC synthetic repeat did not prove per-segment<=500 and global>500");
        }
        input.global_hf_fixture_exact = true;
        input.global_hf_fixture_encoded_bytes = routing_poc_checked_mul(
            input.global_hf_fixture_hash_count,
            16u,
            "global repeat metadata bytes");
        input.global_hf_fixture_chunks = routing_poc_ceil_div(
            input.global_hf_fixture_encoded_bytes,
            bioseq::indexing::kMinimizerExtentPayloadBytes);
    }

    if (!input.simd_proof.accepted(/*require_nonempty_vector_work=*/true)) {
        throw std::runtime_error(
            "routing POC aggregate ExactSimd proof was not accepted");
    }
    input.retained_capacity_bytes =
        routing_poc_retained_capacity_bytes(input);
    std::uint64_t retained_and_scratch = input.retained_capacity_bytes;
    routing_poc_checked_add(
        retained_and_scratch,
        input.measured_scratch_reserved_bytes,
        "retained plus measured scratch bytes");
    std::uint64_t conservative_peak = retained_and_scratch;
    routing_poc_checked_add(
        conservative_peak,
        input.peak_transient_hash_bytes,
        "retained, scratch and transient peak bytes");
    if (conservative_peak > input.admitted_cap_bytes) {
        throw std::runtime_error(fmt::format(
            "routing POC retained/transient capacity {} exceeds admitted cap {}",
            conservative_peak,
            input.admitted_cap_bytes));
    }
    input.retained_permit->shrink_to(retained_and_scratch);
    return input;
}

[[nodiscard]] RoutingPocInput routing_poc_input(
        std::string_view,
        std::string_view test_case_name,
        simdjson::dom::element data,
        const RunConstraints&) {
    RoutingPocInput result;
    dispatch_bench_alphabet(jstr(data, "alphabet", "dna4"),
        [&result, test_case_name, data]<sequence_alphabet A>() {
            result = routing_poc_input_typed<A>(test_case_name, data);
        });
    return result;
}

void cleanup_routing_poc_input(
        std::string_view,
        std::string_view,
        simdjson::dom::element,
        const RoutingPocInput&) {}

[[nodiscard]] bool routing_poc_truth_overlaps(
        const RoutingPocSegment& segment,
        const RoutingPocQuery& query) noexcept {
    return segment.begin_bp < query.truth_end_bp
        && query.truth_begin_bp < segment.end_bp;
}

[[nodiscard]] bool routing_poc_contains_segment(
        std::span<const std::uint32_t> segments,
        std::uint32_t segment) noexcept {
    return std::find(segments.begin(), segments.end(), segment)
        != segments.end();
}

struct RoutingPocBottomScore {
    double containment = 0.0;
    std::uint64_t query_samples = 0;
    std::uint64_t shared_samples = 0;
};

[[nodiscard]] RoutingPocBottomScore routing_poc_bottom_score(
        std::span<const std::uint64_t> target,
        std::span<const std::uint64_t> query,
        std::size_t k) {
    const std::size_t target_count = std::min(k, target.size());
    const std::size_t query_count = std::min(k, query.size());
    if (target_count == 0u || query_count == 0u) {
        return {};
    }
    const std::uint64_t threshold = std::min(
        target[target_count - 1u], query[query_count - 1u]);
    const std::size_t target_below = static_cast<std::size_t>(
        std::upper_bound(
            target.begin(), target.begin()
                + static_cast<std::ptrdiff_t>(target_count), threshold)
        - target.begin());
    const std::size_t query_below = static_cast<std::size_t>(
        std::upper_bound(
            query.begin(), query.begin()
                + static_cast<std::ptrdiff_t>(query_count), threshold)
        - query.begin());
    std::size_t target_cursor = 0;
    std::size_t query_cursor = 0;
    std::uint64_t shared = 0;
    while (target_cursor < target_below && query_cursor < query_below) {
        if (target[target_cursor] < query[query_cursor]) {
            ++target_cursor;
        } else if (query[query_cursor] < target[target_cursor]) {
            ++query_cursor;
        } else {
            ++shared;
            ++target_cursor;
            ++query_cursor;
        }
    }
    return RoutingPocBottomScore{
        .containment = query_below == 0u
            ? 0.0
            : static_cast<double>(shared)
                / static_cast<double>(query_below),
        .query_samples = query_below,
        .shared_samples = shared,
    };
}

void routing_poc_record_decision(
        std::size_t query_ordinal,
        const RoutingPocInput& input,
        const RoutingPocQuery& query,
        std::span<const double> scores,
        std::uint64_t evidence_keys,
        std::uint64_t evidence_hits,
        std::uint64_t minimum_evidence_keys,
        double minimum_score,
        double relative_score,
        std::size_t candidate_cap,
        std::vector<bool>& selected_union,
        std::vector<bool>& query_selected,
        RoutingPocOutput& output) {
    std::vector<std::uint32_t> order(input.segments.size());
    std::iota(order.begin(), order.end(), 0u);
    // The segment ordinal is the explicit tie-break, so a non-allocating sort
    // is deterministic and avoids std::stable_sort's implementation-defined
    // temporary buffer inside the measured/admitted region.
    std::sort(order.begin(), order.end(),
        [scores](std::uint32_t left, std::uint32_t right) noexcept {
            return scores[left] != scores[right]
                ? scores[left] > scores[right] : left < right;
        });
    if (order.empty()) {
        throw std::runtime_error("routing POC has no target segment");
    }

    std::size_t truth_rank = order.size() + 1u;
    std::size_t any_overlap_rank = order.size() + 1u;
    for (std::size_t rank = 0; rank < order.size(); ++rank) {
        if (truth_rank > order.size()
            && routing_poc_contains_segment(
                query.primary_truth_segments, order[rank])) {
            truth_rank = rank + 1u;
        }
        if (any_overlap_rank > order.size()
            && routing_poc_truth_overlaps(
                input.segments[order[rank]], query)) {
            any_overlap_rank = rank + 1u;
        }
    }
    if (truth_rank > order.size() || any_overlap_rank > order.size()) {
        throw std::runtime_error(
            "routing POC primary/overlap truth has no target segment");
    }
    output.truth_rank_sum += truth_rank;
    output.truth_rank_max = std::max<std::uint64_t>(
        output.truth_rank_max, truth_rank);
    output.truth_top1_count += truth_rank == 1u ? 1u : 0u;
    output.truth_top4_count += truth_rank <= 4u ? 1u : 0u;
    output.any_overlap_top1_count += any_overlap_rank == 1u ? 1u : 0u;
    output.any_overlap_top4_count += any_overlap_rank <= 4u ? 1u : 0u;
    routing_poc_checked_add(
        output.sampled_query_keys, evidence_keys, "sampled query keys");
    routing_poc_checked_add(
        output.posting_hits, evidence_hits, "routing evidence hits");
    output.sampled_query_keys_min = output.query_count == 0u
        ? evidence_keys
        : std::min(output.sampled_query_keys_min, evidence_keys);

    const double best = scores[order[0]];
    const double runner_up = order.size() > 1u ? scores[order[1]] : 0.0;
    const double margin = best - runner_up;
    output.margin_sum += margin;
    output.margin_min = std::min(output.margin_min, margin);

    std::vector<std::uint32_t> qualified;
    for (std::uint32_t segment : order) {
        if (scores[segment] < minimum_score
            || scores[segment] < best * relative_score) {
            continue;
        }
        qualified.push_back(segment);
    }
    routing_poc_checked_add(
        output.qualified_candidates,
        qualified.size(),
        "qualified candidates");
    const bool insufficient_evidence = evidence_keys < minimum_evidence_keys;
    const bool weak = best < minimum_score || qualified.empty();
    const bool ambiguous = qualified.size() > candidate_cap;
    const bool fallback = insufficient_evidence || weak || ambiguous;
    output.fallback_queries += fallback ? 1u : 0u;
    output.ambiguity_queries += ambiguous ? 1u : 0u;

    bool truth_qualified = false;
    bool any_overlap_qualified = false;
    for (std::uint32_t segment : qualified) {
        truth_qualified = truth_qualified || routing_poc_contains_segment(
            query.primary_truth_segments, segment);
        any_overlap_qualified = any_overlap_qualified
            || routing_poc_truth_overlaps(input.segments[segment], query);
    }
    const bool material_all_qualified = std::all_of(
        query.material_truth_segments.begin(),
        query.material_truth_segments.end(),
        [&qualified](std::uint32_t material) {
            return std::find(qualified.begin(), qualified.end(), material)
                != qualified.end();
        });
    output.truth_selected_without_fallback_count +=
        !fallback && truth_qualified ? 1u : 0u;
    output.any_overlap_selected_without_fallback_count +=
        !fallback && any_overlap_qualified ? 1u : 0u;
    output.material_all_selected_without_fallback_count +=
        !fallback && material_all_qualified ? 1u : 0u;
    if (query.expects_fallback) {
        ++output.expected_fallback_queries;
        output.expected_fallback_observed += fallback ? 1u : 0u;
        output.expected_fallback_missed += fallback ? 0u : 1u;
    }
    const bool boundary = query.truth_begin_bp / input.segment_bp
        != (query.truth_end_bp - 1u) / input.segment_bp;
    if (boundary) {
        ++output.boundary_query_count;
        output.boundary_truth_selected_count +=
            !fallback && truth_qualified ? 1u : 0u;
    }

    if (fallback) {
        const std::string_view reason = insufficient_evidence
            ? std::string_view{"insufficient_evidence"}
            : weak ? std::string_view{"weak_score"}
                   : std::string_view{"ambiguous_over_cap"};
        bioseq_log<WRN>(
            "[routing_poc] explicit full-scan query={} reason={} evidence={} "
            "best={:.6f} runner_up={:.6f} qualified={} cap={}",
            query_ordinal,
            reason,
            evidence_keys,
            best,
            runner_up,
            qualified.size(),
            candidate_cap);
        std::fill(selected_union.begin(), selected_union.end(), true);
        std::fill(query_selected.begin(), query_selected.end(), true);
        ++output.truth_effective_count;
    } else {
        for (std::uint32_t segment : qualified) {
            selected_union[segment] = true;
            query_selected[segment] = true;
        }
        output.truth_effective_count += truth_qualified ? 1u : 0u;
    }
    ++output.query_count;
}

[[nodiscard]] bool routing_poc_hashes_touch_partition(
        std::span<const std::uint64_t> raw_hashes,
        const RoutingPocPartition& partition) {
    const auto candidate = std::lower_bound(
        raw_hashes.begin(), raw_hashes.end(), partition.raw_lo_hash);
    return candidate != raw_hashes.end()
        && *candidate <= partition.raw_hi_hash;
}

void routing_poc_record_partition_touches(
        const RoutingPocInput& input,
        const std::vector<std::vector<bool>>& query_selected,
        RoutingPocOutput& output) {
    if (query_selected.size() != input.queries.size()) {
        throw std::runtime_error(
            "routing POC query-to-segment assignment shape mismatch");
    }
    for (std::size_t segment_index = 0;
         segment_index < input.segments.size(); ++segment_index) {
        const RoutingPocSegment& segment = input.segments[segment_index];
        for (const RoutingPocPartition& partition : segment.partitions) {
            const bool baseline_touch = routing_poc_hashes_touch_partition(
                input.batch_raw_hashes, partition);
            bool routed_touch = false;
            for (std::size_t query_index = 0;
                 query_index < input.queries.size() && !routed_touch;
                 ++query_index) {
                if (segment_index >= query_selected[query_index].size()
                    || !query_selected[query_index][segment_index]) {
                    continue;
                }
                routed_touch = routing_poc_hashes_touch_partition(
                    input.queries[query_index].raw_hashes, partition);
            }
            if (baseline_touch) {
                ++output.baseline_touched_control_partitions;
                ++output.baseline_touched_position_partitions;
                routing_poc_checked_add(
                    output.baseline_touched_control_chunks,
                    partition.control_chunks,
                    "baseline touched control chunks");
                routing_poc_checked_add(
                    output.baseline_touched_position_chunks,
                    partition.position_chunks,
                    "baseline touched position chunks");
                routing_poc_checked_add(
                    output.baseline_touched_control_bytes,
                    partition.control_bytes,
                    "baseline touched control bytes");
                routing_poc_checked_add(
                    output.baseline_touched_position_bytes,
                    partition.position_bytes,
                    "baseline touched position bytes");
            }
            if (routed_touch) {
                ++output.routed_touched_control_partitions;
                ++output.routed_touched_position_partitions;
                routing_poc_checked_add(
                    output.routed_touched_control_chunks,
                    partition.control_chunks,
                    "routed touched control chunks");
                routing_poc_checked_add(
                    output.routed_touched_position_chunks,
                    partition.position_chunks,
                    "routed touched position chunks");
                routing_poc_checked_add(
                    output.routed_touched_control_bytes,
                    partition.control_bytes,
                    "routed touched control bytes");
                routing_poc_checked_add(
                    output.routed_touched_position_bytes,
                    partition.position_bytes,
                    "routed touched position bytes");
            }
        }
    }
}

[[nodiscard]] RoutingPocOutput routing_poc_run(
        std::string_view algorithm,
        const ParamMap& params,
        const RoutingPocInput& input) {
    if (input.selected_backend != "exact_simd_minimap_v1"
        || !input.simd_proof.accepted(
            /*require_nonempty_vector_work=*/true)
        || input.simd_proof.scalar_backend_branches != 0u
        || input.simd_proof.scalar_hash_symbols != 0u
        || input.simd_proof.scalar_window_transitions != 0u) {
        throw std::runtime_error(
            "routing POC selected ExactSimd backend proof is invalid");
    }
    std::uint64_t required_permit_bytes = input.retained_capacity_bytes;
    routing_poc_checked_add(
        required_permit_bytes,
        input.measured_scratch_reserved_bytes,
        "run permit requirement");
    if (input.retained_permit == nullptr
        || input.retained_permit->bytes() < required_permit_bytes) {
        throw std::runtime_error(
            "routing POC measured scratch is not covered by its retained permit");
    }
    const int cap_value = param_int(params, "candidate_cap", 4);
    const int relative_percent = param_int(params, "relative_percent", 25);
    const int minimum_evidence = param_int(params, "minimum_evidence", 16);
    if (cap_value <= 0 || relative_percent < 0 || relative_percent > 100
        || minimum_evidence < 0) {
        throw std::invalid_argument(
            "routing POC candidate thresholds are invalid");
    }
    const std::size_t candidate_cap = static_cast<std::size_t>(cap_value);
    const double relative_score = static_cast<double>(relative_percent) / 100.0;
    const std::uint64_t minimum_evidence_keys =
        static_cast<std::uint64_t>(minimum_evidence);

    RoutingPocOutput output;
    // One benchmark input is one cold multi-query batch. The selected-byte
    // metrics therefore count the union of segments needed by all queries,
    // matching a loader that materializes each immutable segment at most once
    // for the batch rather than summing repeated per-query loads.
    std::vector<bool> selected_union(input.segments.size(), false);
    std::vector<std::vector<bool>> query_selected(
        input.queries.size(),
        std::vector<bool>(input.segments.size(), false));
    const auto start_ns = static_cast<std::uint64_t>(
        bioseq::instr::clock::now_ns());
    {
        auto scope = bioseq::instr::region(kRoutingPocMeasuredRegion);
        if (algorithm == "routing_sampled_postings") {
            const int requested_rate = param_int(params, "sample_rate", 2048);
            if (requested_rate <= 0
                || !routing_poc_power_of_two(
                    static_cast<std::uint32_t>(requested_rate))) {
                throw std::invalid_argument(
                    "routing POC sample rate must be a power of two");
            }
            const auto sample = std::find_if(
                input.samples.begin(), input.samples.end(),
                [requested_rate](const RoutingPocSampleIndex& candidate) {
                    return candidate.rate
                        == static_cast<std::uint32_t>(requested_rate);
                });
            if (sample == input.samples.end()) {
                throw std::invalid_argument(
                    "routing POC sample rate was not precomputed");
            }
            const int minimum_votes = param_int(params, "minimum_votes", 32);
            if (minimum_votes < 0) {
                throw std::invalid_argument(
                    "routing POC minimum_votes is negative");
            }
            output.sampled_router = true;
            output.sample_rate = sample->rate;
            for (std::size_t query_ordinal = 0;
                 query_ordinal < input.queries.size(); ++query_ordinal) {
                const RoutingPocQuery& query = input.queries[query_ordinal];
                std::vector<std::uint32_t> votes(input.segments.size(), 0u);
                std::uint64_t query_keys = 0;
                std::uint64_t posting_hits = 0;
                for (std::uint64_t hash : query.routing_hashes) {
                    if ((hash & (sample->rate - 1u)) != 0u) {
                        continue;
                    }
                    ++query_keys;
                    const RoutingPocPosting key{
                        .routing_hash = hash,
                        .segment = 0u,
                    };
                    auto posting = std::lower_bound(
                        sample->postings.begin(), sample->postings.end(), key,
                        [](const RoutingPocPosting& left,
                           const RoutingPocPosting& right) noexcept {
                            return left < right;
                        });
                    while (posting != sample->postings.end()
                           && posting->routing_hash == hash) {
                        if (votes[posting->segment]
                            == (std::numeric_limits<std::uint32_t>::max)()) {
                            throw std::overflow_error(
                                "routing POC vote counter overflow");
                        }
                        ++votes[posting->segment];
                        ++posting_hits;
                        ++posting;
                    }
                }
                std::vector<double> scores(votes.begin(), votes.end());
                routing_poc_record_decision(
                    query_ordinal,
                    input,
                    query,
                    scores,
                    query_keys,
                    posting_hits,
                    minimum_evidence_keys,
                    static_cast<double>(minimum_votes),
                    relative_score,
                    candidate_cap,
                    selected_union,
                    query_selected[query_ordinal],
                    output);
            }
        } else if (algorithm == "routing_bottom_k") {
            const int requested_k = param_int(params, "bottom_k", 256);
            const int minimum_score_percent =
                param_int(params, "minimum_score_percent", 10);
            if ((requested_k != 64 && requested_k != 128
                    && requested_k != 256 && requested_k != 512)
                || minimum_score_percent < 0
                || minimum_score_percent > 100) {
                throw std::invalid_argument(
                    "routing POC bottom-K parameters are invalid");
            }
            output.bottom_k = static_cast<std::uint64_t>(requested_k);
            const double minimum_score =
                static_cast<double>(minimum_score_percent) / 100.0;
            for (std::size_t query_ordinal = 0;
                 query_ordinal < input.queries.size(); ++query_ordinal) {
                const RoutingPocQuery& query = input.queries[query_ordinal];
                std::vector<double> scores(input.segments.size(), 0.0);
                std::uint64_t shared = 0;
                std::uint64_t query_samples = std::min<std::uint64_t>(
                    requested_k, query.routing_hashes.size());
                for (std::size_t segment = 0;
                     segment < input.segments.size(); ++segment) {
                    const RoutingPocBottomScore score = routing_poc_bottom_score(
                        input.segments[segment].bottom_k,
                        query.routing_hashes,
                        static_cast<std::size_t>(requested_k));
                    scores[segment] = score.containment;
                    shared += score.shared_samples;
                    query_samples = std::min(
                        query_samples, score.query_samples);
                }
                routing_poc_record_decision(
                    query_ordinal,
                    input,
                    query,
                    scores,
                    query_samples,
                    shared,
                    minimum_evidence_keys,
                    minimum_score,
                    relative_score,
                    candidate_cap,
                    selected_union,
                    query_selected[query_ordinal],
                    output);
            }
        } else {
            throw std::invalid_argument(fmt::format(
                "unknown routing POC algorithm '{}'", algorithm));
        }
        if (output.expected_fallback_missed != 0u) {
            throw std::runtime_error(fmt::format(
                "routing POC missed {} explicitly expected adversarial fallback(s)",
                output.expected_fallback_missed));
        }
    }
    const auto router_end_ns = static_cast<std::uint64_t>(
        bioseq::instr::clock::now_ns());
    output.measured_region_ns = router_end_ns - start_ns;
    const auto coverage_start_ns = static_cast<std::uint64_t>(
        bioseq::instr::clock::now_ns());
    routing_poc_record_partition_touches(
        input, query_selected, output);
    const auto coverage_end_ns = static_cast<std::uint64_t>(
        bioseq::instr::clock::now_ns());
    output.coverage_analysis_ns = coverage_end_ns - coverage_start_ns;
    if (!std::isfinite(output.margin_min)) {
        output.margin_min = 0.0;
    }
    for (std::size_t segment = 0;
         segment < input.segments.size(); ++segment) {
        if (!selected_union[segment]) {
            continue;
        }
        const RoutingPocSegment& selected = input.segments[segment];
        ++output.selected_segments;
        routing_poc_checked_add(
            output.selected_segment_bp,
            selected.end_bp - selected.begin_bp,
            "selected segment bp");
        routing_poc_checked_add(
            output.selected_control_bytes,
            selected.control_bytes,
            "selected control bytes");
        routing_poc_checked_add(
            output.selected_position_bytes,
            selected.position_bytes,
            "selected position bytes");
        routing_poc_checked_add(
            output.selected_payload_chunks,
            selected.payload_chunks,
            "selected payload chunks");
        routing_poc_checked_add(
            output.selected_payload_rows,
            selected.payload_chunks,
            "selected payload rows");
        routing_poc_checked_add(
            output.selected_partitions,
            selected.partition_count,
            "selected partitions");
    }
    bioseq_log<INF>(
        "[routing_poc] algorithm={} queries={} truth_top1={}/{} "
        "truth_selected={}/{} fallbacks={} selected_segments={}/{} "
        "selected_bp={} selected_payload_bytes={}",
        algorithm,
        output.query_count,
        output.truth_top1_count,
        output.query_count,
        output.truth_selected_without_fallback_count,
        output.query_count,
        output.fallback_queries,
        output.selected_segments,
        input.segments.size(),
        output.selected_segment_bp,
        output.selected_control_bytes + output.selected_position_bytes);
    return output;
}

[[nodiscard]] std::map<std::string, double> routing_poc_eval(
        std::string_view,
        const ParamMap&,
        const RoutingPocInput& input,
        const RoutingPocOutput& output) {
    const double queries = static_cast<double>(output.query_count);
    const std::uint64_t total_payload_bytes =
        input.total_control_bytes + input.total_position_bytes;
    const std::uint64_t selected_payload_bytes =
        output.selected_control_bytes + output.selected_position_bytes;
    const std::uint64_t total_storage_wire_bytes = total_payload_bytes
        + input.directory_bytes + kRoutingPocGenerationFixedWireBytes;
    const std::uint64_t total_storage_rows = input.total_payload_rows
        + input.directory_rows + 2u;
    const std::uint64_t baseline_touched_payload_bytes =
        output.baseline_touched_control_bytes
        + output.baseline_touched_position_bytes;
    const std::uint64_t routed_touched_payload_bytes =
        output.routed_touched_control_bytes
        + output.routed_touched_position_bytes;
    const std::uint64_t fixed_read_bytes = input.directory_bytes
        + kRoutingPocGenerationFixedWireBytes;
    const std::uint64_t baseline_cold_read_model_bytes =
        fixed_read_bytes + baseline_touched_payload_bytes;
    const std::uint64_t routed_cold_read_model_bytes =
        fixed_read_bytes + routed_touched_payload_bytes;
    if (input.total_control_bytes % 16u != 0u
        || input.total_position_bytes % 8u != 0u) {
        throw std::runtime_error(
            "routing POC current-wire counters are not element aligned");
    }
    const std::uint64_t unique_hash_count = input.total_control_bytes / 16u;
    const std::uint64_t minimizer_event_count = input.total_position_bytes / 8u;
    std::uint64_t compact_grouped64_local32_payload_bytes =
        routing_poc_checked_mul(unique_hash_count, 16u,
            "compact grouped64 controls");
    routing_poc_checked_add(
        compact_grouped64_local32_payload_bytes,
        routing_poc_checked_mul(minimizer_event_count, 4u,
            "compact grouped64 positions"),
        "compact grouped64 payload");
    std::uint64_t compact_grouped32_local32_payload_bytes =
        routing_poc_checked_mul(unique_hash_count, 12u,
            "compact grouped32 controls");
    routing_poc_checked_add(
        compact_grouped32_local32_payload_bytes,
        routing_poc_checked_mul(minimizer_event_count, 4u,
            "compact grouped32 positions"),
        "compact grouped32 payload");
    const std::uint64_t compact_flat64_local32_payload_bytes =
        routing_poc_checked_mul(minimizer_event_count, 12u,
            "compact flat64 records");
    const std::uint64_t compact_flat32_local32_payload_bytes =
        routing_poc_checked_mul(minimizer_event_count, 8u,
            "compact flat32 records");
    std::uint64_t compact_prefix16_suffix48_local32_payload_bytes =
        routing_poc_checked_mul(minimizer_event_count, 10u,
            "compact suffix48 records");
    routing_poc_checked_add(
        compact_prefix16_suffix48_local32_payload_bytes,
        routing_poc_checked_mul(input.segments.size(),
            kRoutingPocPrefix16OffsetBytes,
            "compact prefix16 directories"),
        "compact prefix16 payload");
    std::uint64_t compact_grouped64_local32_rows = 2u;
    std::uint64_t compact_grouped32_local32_rows = 2u;
    std::uint64_t compact_flat64_local32_rows = 2u;
    std::uint64_t compact_prefix16_suffix48_local32_rows = 2u;
    std::uint64_t compact_flat32_local32_rows = 2u;
    for (const RoutingPocSegment& segment : input.segments) {
        std::uint64_t grouped_segment_bytes = routing_poc_checked_mul(
            segment.control_count, 16u,
            "compact grouped64 segment controls");
        routing_poc_checked_add(
            grouped_segment_bytes,
            routing_poc_checked_mul(segment.position_count, 4u,
                "compact grouped64 segment positions"),
            "compact grouped64 segment payload");
        routing_poc_checked_add(
            compact_grouped64_local32_rows,
            routing_poc_ceil_div(
                grouped_segment_bytes,
                bioseq::indexing::kMinimizerExtentPayloadBytes),
            "compact grouped64 rows");
        std::uint64_t grouped32_segment_bytes = routing_poc_checked_mul(
            segment.control_count, 12u,
            "compact grouped32 segment controls");
        routing_poc_checked_add(
            grouped32_segment_bytes,
            routing_poc_checked_mul(segment.position_count, 4u,
                "compact grouped32 segment positions"),
            "compact grouped32 segment payload");
        routing_poc_checked_add(
            compact_grouped32_local32_rows,
            routing_poc_ceil_div(
                grouped32_segment_bytes,
                bioseq::indexing::kMinimizerExtentPayloadBytes),
            "compact grouped32 rows");
        routing_poc_checked_add(
            compact_flat64_local32_rows,
            routing_poc_ceil_div(
                routing_poc_checked_mul(segment.position_count, 12u,
                    "compact flat64 segment records"),
                bioseq::indexing::kMinimizerExtentPayloadBytes),
            "compact flat64 rows");
        routing_poc_checked_add(
            compact_prefix16_suffix48_local32_rows,
            routing_poc_ceil_div(
                routing_poc_checked_mul(segment.position_count, 10u,
                    "compact suffix48 segment records"),
                bioseq::indexing::kMinimizerExtentPayloadBytes),
            "compact suffix48 rows");
        routing_poc_checked_add(
            compact_prefix16_suffix48_local32_rows,
            routing_poc_ceil_div(kRoutingPocPrefix16OffsetBytes,
                bioseq::indexing::kMinimizerExtentPayloadBytes),
            "compact prefix16 rows");
        routing_poc_checked_add(
            compact_flat32_local32_rows,
            routing_poc_ceil_div(
                routing_poc_checked_mul(segment.position_count, 8u,
                    "compact flat32 segment records"),
                bioseq::indexing::kMinimizerExtentPayloadBytes),
            "compact flat32 rows");
    }
    std::uint64_t raw_query_hashes_total = 0;
    for (const RoutingPocQuery& query : input.queries) {
        routing_poc_checked_add(
            raw_query_hashes_total,
            query.raw_hashes.size(),
            "evaluation raw query hashes");
    }
    return {
        {"query_count", queries},
        {"forced_query_count", static_cast<double>(input.forced_query_count)},
        {"raw_query_hashes_total", static_cast<double>(
            raw_query_hashes_total)},
        {"batch_raw_hash_union_count", static_cast<double>(
            input.batch_raw_hashes.size())},
        {"truth_top1_recall", queries == 0.0 ? 0.0
            : static_cast<double>(output.truth_top1_count) / queries},
        {"primary_truth_top1_recall", queries == 0.0 ? 0.0
            : static_cast<double>(output.truth_top1_count) / queries},
        {"truth_top4_recall", queries == 0.0 ? 0.0
            : static_cast<double>(output.truth_top4_count) / queries},
        {"primary_truth_top4_recall", queries == 0.0 ? 0.0
            : static_cast<double>(output.truth_top4_count) / queries},
        {"any_overlap_truth_top1_recall", queries == 0.0 ? 0.0
            : static_cast<double>(output.any_overlap_top1_count) / queries},
        {"any_overlap_truth_top4_recall", queries == 0.0 ? 0.0
            : static_cast<double>(output.any_overlap_top4_count) / queries},
        {"truth_selected_without_fallback_recall", queries == 0.0 ? 0.0
            : static_cast<double>(
                output.truth_selected_without_fallback_count) / queries},
        {"primary_truth_selected_without_fallback_recall",
            queries == 0.0 ? 0.0
            : static_cast<double>(
                output.truth_selected_without_fallback_count) / queries},
        {"any_overlap_selected_without_fallback_recall",
            queries == 0.0 ? 0.0
            : static_cast<double>(
                output.any_overlap_selected_without_fallback_count) / queries},
        {"material_all_selected_without_fallback_recall",
            queries == 0.0 ? 0.0
            : static_cast<double>(
                output.material_all_selected_without_fallback_count) / queries},
        {"truth_effective_recall", queries == 0.0 ? 0.0
            : static_cast<double>(output.truth_effective_count) / queries},
        {"truth_rank_mean", queries == 0.0 ? 0.0
            : static_cast<double>(output.truth_rank_sum) / queries},
        {"truth_rank_max", static_cast<double>(output.truth_rank_max)},
        {"margin_mean", queries == 0.0 ? 0.0
            : output.margin_sum / queries},
        {"margin_min", output.margin_min},
        {"sampled_query_keys", static_cast<double>(output.sampled_query_keys)},
        {"sampled_query_keys_min", static_cast<double>(
            output.sampled_query_keys_min)},
        {"posting_hits", static_cast<double>(output.posting_hits)},
        {"qualified_candidates", static_cast<double>(
            output.qualified_candidates)},
        {"fallback_queries", static_cast<double>(output.fallback_queries)},
        {"ambiguity_queries", static_cast<double>(output.ambiguity_queries)},
        {"expected_fallback_queries", static_cast<double>(
            output.expected_fallback_queries)},
        {"expected_fallback_observed", static_cast<double>(
            output.expected_fallback_observed)},
        {"expected_fallback_missed", static_cast<double>(
            output.expected_fallback_missed)},
        {"selected_segments", static_cast<double>(output.selected_segments)},
        {"whole_segment_upper_bound_selected_segments", static_cast<double>(
            output.selected_segments)},
        {"total_segments", static_cast<double>(input.segments.size())},
        {"selected_segment_bp", static_cast<double>(
            output.selected_segment_bp)},
        {"whole_segment_upper_bound_selected_bp", static_cast<double>(
            output.selected_segment_bp)},
        {"selected_target_fraction", input.target_bp == 0u ? 0.0
            : static_cast<double>(output.selected_segment_bp)
                / static_cast<double>(input.target_bp)},
        {"selected_control_bytes", static_cast<double>(
            output.selected_control_bytes)},
        {"whole_segment_upper_bound_selected_control_bytes",
            static_cast<double>(output.selected_control_bytes)},
        {"selected_position_bytes", static_cast<double>(
            output.selected_position_bytes)},
        {"whole_segment_upper_bound_selected_position_bytes",
            static_cast<double>(output.selected_position_bytes)},
        {"selected_payload_bytes", static_cast<double>(
            selected_payload_bytes)},
        {"whole_segment_upper_bound_selected_payload_bytes",
            static_cast<double>(selected_payload_bytes)},
        {"total_payload_bytes", static_cast<double>(total_payload_bytes)},
        {"selected_payload_fraction", total_payload_bytes == 0u ? 0.0
            : static_cast<double>(selected_payload_bytes)
                / static_cast<double>(total_payload_bytes)},
        {"selected_payload_chunks", static_cast<double>(
            output.selected_payload_chunks)},
        {"whole_segment_upper_bound_selected_payload_chunks",
            static_cast<double>(output.selected_payload_chunks)},
        {"selected_payload_rows", static_cast<double>(
            output.selected_payload_rows)},
        {"whole_segment_upper_bound_selected_payload_rows",
            static_cast<double>(output.selected_payload_rows)},
        {"selected_partitions", static_cast<double>(
            output.selected_partitions)},
        {"whole_segment_upper_bound_selected_partitions",
            static_cast<double>(output.selected_partitions)},
        {"baseline_touched_control_partitions", static_cast<double>(
            output.baseline_touched_control_partitions)},
        {"baseline_touched_position_partitions", static_cast<double>(
            output.baseline_touched_position_partitions)},
        {"baseline_touched_control_chunks", static_cast<double>(
            output.baseline_touched_control_chunks)},
        {"baseline_touched_position_chunks", static_cast<double>(
            output.baseline_touched_position_chunks)},
        {"baseline_touched_control_bytes", static_cast<double>(
            output.baseline_touched_control_bytes)},
        {"baseline_touched_position_bytes", static_cast<double>(
            output.baseline_touched_position_bytes)},
        {"baseline_touched_payload_bytes", static_cast<double>(
            baseline_touched_payload_bytes)},
        {"routed_touched_control_partitions", static_cast<double>(
            output.routed_touched_control_partitions)},
        {"routed_touched_position_partitions", static_cast<double>(
            output.routed_touched_position_partitions)},
        {"routed_touched_control_chunks", static_cast<double>(
            output.routed_touched_control_chunks)},
        {"routed_touched_position_chunks", static_cast<double>(
            output.routed_touched_position_chunks)},
        {"routed_touched_control_bytes", static_cast<double>(
            output.routed_touched_control_bytes)},
        {"routed_touched_position_bytes", static_cast<double>(
            output.routed_touched_position_bytes)},
        {"routed_touched_payload_bytes", static_cast<double>(
            routed_touched_payload_bytes)},
        {"routed_vs_baseline_touched_payload_fraction",
            baseline_touched_payload_bytes == 0u ? 0.0
            : static_cast<double>(routed_touched_payload_bytes)
                / static_cast<double>(baseline_touched_payload_bytes)},
        {"baseline_cold_read_model_bytes", static_cast<double>(
            baseline_cold_read_model_bytes)},
        {"routed_cold_read_model_bytes", static_cast<double>(
            routed_cold_read_model_bytes)},
        {"routed_vs_baseline_cold_read_fraction",
            baseline_cold_read_model_bytes == 0u ? 0.0
            : static_cast<double>(routed_cold_read_model_bytes)
                / static_cast<double>(baseline_cold_read_model_bytes)},
        {"total_payload_chunks", static_cast<double>(
            input.total_payload_chunks)},
        {"total_payload_rows", static_cast<double>(input.total_payload_rows)},
        {"total_partitions", static_cast<double>(input.total_partitions)},
        {"directory_entry_bytes", static_cast<double>(
            kRoutingPocDirectoryEntryBytes)},
        {"directory_bytes", static_cast<double>(input.directory_bytes)},
        {"directory_chunks", static_cast<double>(input.directory_chunks)},
        {"directory_rows", static_cast<double>(input.directory_rows)},
        {"generation_head_manifest_bytes", static_cast<double>(
            kRoutingPocGenerationFixedWireBytes)},
        {"generation_head_manifest_rows", 2.0},
        {"total_storage_wire_bytes", static_cast<double>(
            total_storage_wire_bytes)},
        {"total_storage_rows", static_cast<double>(total_storage_rows)},
        {"minimizer_event_count", static_cast<double>(
            minimizer_event_count)},
        {"unique_minimizer_hash_count", static_cast<double>(
            unique_hash_count)},
        {"compact_grouped64_local32_payload_bytes", static_cast<double>(
            compact_grouped64_local32_payload_bytes)},
        {"compact_grouped64_local32_storage_rows_6k", static_cast<double>(
            compact_grouped64_local32_rows)},
        {"compact_grouped64_local32_vs_current_wire_fraction",
            total_storage_wire_bytes == 0u ? 0.0
            : static_cast<double>(compact_grouped64_local32_payload_bytes
                    + kRoutingPocGenerationFixedWireBytes)
                / static_cast<double>(total_storage_wire_bytes)},
        {"compact_grouped32_local32_payload_bytes", static_cast<double>(
            compact_grouped32_local32_payload_bytes)},
        {"compact_grouped32_local32_storage_rows_6k", static_cast<double>(
            compact_grouped32_local32_rows)},
        {"compact_grouped32_local32_vs_current_wire_fraction",
            total_storage_wire_bytes == 0u ? 0.0
            : static_cast<double>(compact_grouped32_local32_payload_bytes
                    + kRoutingPocGenerationFixedWireBytes)
                / static_cast<double>(total_storage_wire_bytes)},
        {"compact_flat64_local32_payload_bytes", static_cast<double>(
            compact_flat64_local32_payload_bytes)},
        {"compact_flat64_local32_storage_rows_6k", static_cast<double>(
            compact_flat64_local32_rows)},
        {"compact_flat64_local32_vs_current_wire_fraction",
            total_storage_wire_bytes == 0u ? 0.0
            : static_cast<double>(compact_flat64_local32_payload_bytes
                    + kRoutingPocGenerationFixedWireBytes)
                / static_cast<double>(total_storage_wire_bytes)},
        {"compact_prefix16_suffix48_local32_payload_bytes",
            static_cast<double>(
                compact_prefix16_suffix48_local32_payload_bytes)},
        {"compact_prefix16_suffix48_local32_storage_rows_6k",
            static_cast<double>(
                compact_prefix16_suffix48_local32_rows)},
        {"compact_prefix16_suffix48_local32_vs_current_wire_fraction",
            total_storage_wire_bytes == 0u ? 0.0
            : static_cast<double>(
                compact_prefix16_suffix48_local32_payload_bytes
                    + kRoutingPocGenerationFixedWireBytes)
                / static_cast<double>(total_storage_wire_bytes)},
        {"compact_flat32_local32_payload_bytes", static_cast<double>(
            compact_flat32_local32_payload_bytes)},
        {"compact_flat32_local32_storage_rows_6k",
            static_cast<double>(compact_flat32_local32_rows)},
        {"compact_flat32_local32_vs_current_wire_fraction",
            total_storage_wire_bytes == 0u ? 0.0
            : static_cast<double>(compact_flat32_local32_payload_bytes
                    + kRoutingPocGenerationFixedWireBytes)
                / static_cast<double>(total_storage_wire_bytes)},
        {"compact_layout_model_only", 1.0},
        {"compact_grouped64_exact_hash", 1.0},
        {"compact_grouped32_scheme_validated_exact_hash", 1.0},
        {"compact_flat64_exact_hash", 1.0},
        {"compact_prefix16_suffix48_exact_hash", 1.0},
        {"compact_flat32_scheme_validated_exact_hash", 1.0},
        {"compact_grouped64_preserves_two_phase_counts", 1.0},
        {"compact_grouped32_preserves_two_phase_counts", 1.0},
        {"compact_flat64_preserves_two_phase_counts", 0.0},
        {"compact_prefix16_suffix48_preserves_two_phase_counts", 0.0},
        {"compact_flat32_preserves_two_phase_counts", 0.0},
        {"boundary_query_count", static_cast<double>(
            output.boundary_query_count)},
        {"boundary_truth_selected_count", static_cast<double>(
            output.boundary_truth_selected_count)},
        {"minimum_truth_overlap_bp", static_cast<double>(
            input.minimum_truth_overlap_bp)},
        {"material_overlap_percent", static_cast<double>(
            kRoutingPocMaterialOverlapPercent)},
        {"sample_rate", static_cast<double>(output.sample_rate)},
        {"bottom_k", static_cast<double>(output.bottom_k)},
        {"router_sampled_postings", output.sampled_router ? 1.0 : 0.0},
        {"router_bottom_k", output.sampled_router ? 0.0 : 1.0},
        {"selection_scope_batch_union", 1.0},
        {"whole_segment_upper_bound_model", 1.0},
        {"partition_directory_range_touch_model", 1.0},
        {"position_partition_payload_upper_bound_model", 1.0},
        {"router_wire_codec_model_only", 1.0},
        {"production_loader_wired", 0.0},
        {"production_decision_acceptance_enabled", 0.0},
        {"global_hf_full_scale_blocked",
            input.global_hf_full_scale_blocked ? 1.0 : 0.0},
        {"global_hf_candidate_fixture_only",
            input.global_hf_fixture_exact ? 1.0 : 0.0},
        {"global_hf_candidate_fixture_set_retained",
            input.global_hf_fixture_exact
                && input.global_hf_fixture_hashes.size()
                    == input.global_hf_fixture_hash_count ? 1.0 : 0.0},
        {"global_hf_candidate_fixture_hash_count", static_cast<double>(
            input.global_hf_fixture_hash_count)},
        {"global_hf_candidate_fixture_encoded_bytes", static_cast<double>(
            input.global_hf_fixture_encoded_bytes)},
        {"global_hf_candidate_fixture_chunks", static_cast<double>(
            input.global_hf_fixture_chunks)},
        {"sampled_router_wire_bytes_rate_1024", static_cast<double>(
            input.sampled_router_wire_bytes[0])},
        {"sampled_router_wire_bytes_rate_2048", static_cast<double>(
            input.sampled_router_wire_bytes[1])},
        {"sampled_router_wire_bytes_rate_4096", static_cast<double>(
            input.sampled_router_wire_bytes[2])},
        {"sampled_router_wire_bytes_rate_8192", static_cast<double>(
            input.sampled_router_wire_bytes[3])},
        {"bottom_k_router_wire_bytes_k64", static_cast<double>(
            input.bottom_k_router_wire_bytes[0])},
        {"bottom_k_router_wire_bytes_k128", static_cast<double>(
            input.bottom_k_router_wire_bytes[1])},
        {"bottom_k_router_wire_bytes_k256", static_cast<double>(
            input.bottom_k_router_wire_bytes[2])},
        {"bottom_k_router_wire_bytes_k512", static_cast<double>(
            input.bottom_k_router_wire_bytes[3])},
        {"backend_exact_simd", 1.0},
        {"backend_exact_minimap_scheme", 1.0},
        {"backend_paper_nthash_scheme", 0.0},
        {"paper_route_transfer_claim", 0.0},
        {"backend_scalar_fallback_branches", static_cast<double>(
            input.simd_proof.scalar_backend_branches)},
        {"backend_scalar_hash_symbols", static_cast<double>(
            input.simd_proof.scalar_hash_symbols)},
        {"simd_vector_batches", static_cast<double>(
            input.simd_proof.vector_batches)},
        {"simd_proof_accepted", input.simd_proof.accepted(true) ? 1.0 : 0.0},
        {"retained_capacity_bytes", static_cast<double>(
            input.retained_capacity_bytes)},
        {"measured_scratch_reserved_bytes", static_cast<double>(
            input.measured_scratch_reserved_bytes)},
        {"fixture_clone_peak_bytes", static_cast<double>(
            input.fixture_clone_peak_bytes)},
        {"peak_transient_hash_bytes", static_cast<double>(
            input.peak_transient_hash_bytes)},
        {"admitted_cap_bytes", static_cast<double>(input.admitted_cap_bytes)},
        {"measured_region_ns_manual", static_cast<double>(
            output.measured_region_ns)},
        {"coverage_analysis_ns_manual", static_cast<double>(
            output.coverage_analysis_ns)},
    };
}

struct SortScanInput {
    std::shared_ptr<it::connection_test_context> conn;
    std::string table;
    std::string seq_type = "bioseq.seq('dna4', 64)";
    std::vector<long long> expected_pks;
    std::size_t row_count = 0;
    std::size_t sequence_length = 0;
};

struct SortScanOutput {
    std::int64_t row_count = 0;
    std::int64_t sorted_correct = 0;
    std::int64_t custom_scan_taken = 0;
    std::int64_t accelerator_gpu = 0;
    std::int64_t accelerator_stdc_compare = 0;
    std::int64_t gpu_sort_dispatches = 0;
    std::int64_t gpu_global_compare_dispatches = 0;
    std::int64_t gpu_dense_scatter_dispatches = 0;
    std::int64_t gpu_sort_uploaded_bytes = 0;
    std::int64_t cpu_refine_rows = 0;
    std::int64_t sort_runtime_ns = 0;
    std::uint64_t measured_region_ns = 0;
};

[[nodiscard]] std::string sort_scan_identifier(std::string_view raw) {
    std::string out;
    out.reserve(raw.size() + 16u);
    for (char ch : raw) {
        if (ch >= 'A' && ch <= 'Z') {
            out.push_back(static_cast<char>(ch - 'A' + 'a'));
        } else if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) {
            out.push_back(ch);
        } else {
            out.push_back('_');
        }
    }
    if (out.empty() || (out.front() >= '0' && out.front() <= '9')) {
        out.insert(out.begin(), 's');
    }
    return out;
}

[[nodiscard]] std::int64_t sort_scan_explain_int(
    std::span<const std::string> plan,
    std::string_view key,
    std::int64_t fallback = -1)
{
    const auto value = it::find_explain_property(plan, key);
    if (!value.has_value()) return fallback;
    return std::stoll(*value);
}

[[nodiscard]] RunConstraints sort_scan_estimate(
    std::string_view, std::string_view, simdjson::dom::element data)
{
    const std::size_t rows = static_cast<std::size_t>(jint(data, "row_count", 65536));
    const std::size_t seq_len = static_cast<std::size_t>(jint(data, "sequence_length", 32));
    const std::size_t packed_bytes = (seq_len + 3u) / 4u;
    return {
        .base_memory = rows * (packed_bytes + 64u),
        .extra_memory = rows * (packed_bytes + 32u),
    };
}

[[nodiscard]] SortScanInput sort_scan_input(
    std::string_view,
    std::string_view test_case_name,
    simdjson::dom::element data,
    const RunConstraints&)
{
    SortScanInput in;
    in.conn = std::make_shared<it::connection_test_context>();
    in.table = "bench_sort_scan_" + sort_scan_identifier(test_case_name);
    in.row_count = static_cast<std::size_t>(jint(data, "row_count", 65536));
    in.sequence_length = static_cast<std::size_t>(jint(data, "sequence_length", 32));
    const std::size_t prefix_len = static_cast<std::size_t>(jint(data, "prefix_length", 0));
    const std::size_t key_digits = static_cast<std::size_t>(jint(data, "key_digits", 32));
    const std::int64_t multiplier = jint(data, "permutation_multiplier", 32771);
    if (in.sequence_length < prefix_len + key_digits) {
        bioseq_log<ERR>(
            "[bench_sort_scan] invalid case {}: sequence_length={} prefix_length={} key_digits={}",
            test_case_name,
            in.sequence_length,
            prefix_len,
            key_digits);
        std::abort();
    }
    const std::size_t tail_len = in.sequence_length - prefix_len - key_digits;

    in.expected_pks.assign(in.row_count, -1);
    for (std::size_t pk = 0; pk < in.row_count; ++pk) {
        const std::size_t key = static_cast<std::size_t>(
            (static_cast<std::int64_t>(pk) * multiplier)
            % static_cast<std::int64_t>(in.row_count));
        if (key >= in.row_count || in.expected_pks[key] != -1) {
            bioseq_log<ERR>(
                "[bench_sort_scan] permutation_multiplier={} is not a permutation for row_count={}",
                multiplier,
                in.row_count);
            std::abort();
        }
        in.expected_pks[key] = static_cast<long long>(pk);
    }
    it::exec_sqlf(*in.conn, "DROP TABLE IF EXISTS {} CASCADE;", in.table);
    it::exec_sql(
        *in.conn,
        "CREATE OR REPLACE FUNCTION pg_temp.bioseq_sort_scan_dna4_text("
        "v bigint, key_digits integer, prefix_len integer, tail_len integer) "
        "RETURNS text LANGUAGE SQL IMMUTABLE AS $$ "
        "SELECT repeat('A', prefix_len) || "
        "       (SELECT string_agg(substr('ACGT', ((((v >> ((2 * g)::integer)) & 3) + 1)::integer), 1), '' "
        "                          ORDER BY g DESC) "
        "        FROM generate_series(0, key_digits - 1) AS gs(g)) || "
        "       repeat('C', tail_len) $$;");
    it::exec_sqlf(
        *in.conn,
        "CREATE TABLE {} (pk bigint PRIMARY KEY, seq0 {});",
        in.table,
        in.seq_type);
    disable_e2e_autovacuum(*in.conn, in.table);
    it::exec_sqlf(
        *in.conn,
        "INSERT INTO {} "
        "SELECT i::bigint, "
        "       pg_temp.bioseq_sort_scan_dna4_text("
        "           ((i::bigint * {}) % {})::bigint, {}, {}, {})::{} "
        "FROM generate_series(0, {}::bigint - 1) AS g(i);",
        in.table,
        multiplier,
        static_cast<unsigned long long>(in.row_count),
        static_cast<unsigned long long>(key_digits),
        static_cast<unsigned long long>(prefix_len),
        static_cast<unsigned long long>(tail_len),
        in.seq_type,
        static_cast<unsigned long long>(in.row_count));
    it::exec_sqlf(*in.conn, "ANALYZE {};", in.table);
    return in;
}

void cleanup_sort_scan_input(
    std::string_view,
    std::string_view,
    simdjson::dom::element,
    const SortScanInput& in)
{
    const char* keep_artifacts_env = std::getenv("BIOSEQ_E2E_KEEP_ARTIFACTS");
    if (keep_artifacts_env != nullptr && std::string_view{keep_artifacts_env} == "1") {
        return;
    }
    if (in.conn != nullptr) {
        it::exec_sqlf(*in.conn, "DROP TABLE IF EXISTS {} CASCADE;", in.table);
    }
}

[[nodiscard]] SortScanOutput sort_scan_run(
    std::string_view algo,
    const ParamMap& params,
    const SortScanInput& in)
{
    SortScanOutput out;
    out.row_count = static_cast<std::int64_t>(in.row_count);
    const std::string implementation = param_string(params, "implementation", "gpu");
    const bool use_gpu = implementation == "gpu" || algo == "sort_scan_gpu";
    it::exec_sql(*in.conn, "SET bioseq.enable_sequence_sort_scan = on;");
    it::exec_sqlf(*in.conn, "SET bioseq.aligner.vulkan_enabled = {};",
                  use_gpu ? "on" : "off");

    const std::string sql = fmt::format("SELECT pk FROM {} ORDER BY seq0;", in.table);
    ScopedResourceMonitor resources;
    const auto start_ns = static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
    std::vector<long long> pks;
    {
        auto _scope = bioseq::instr::region(kMeasuredRegion);
        pks = it::query_int_column(*in.conn, sql);
    }
    const auto end_ns = static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
    (void)resources.stop();
    out.measured_region_ns = end_ns - start_ns;
    out.sorted_correct = (pks == in.expected_pks) ? 1 : 0;
    if (out.sorted_correct == 0) {
        bioseq_log<ERR>(
            "[bench_sort_scan] correctness failure algo={} got={} expected={}",
            algo,
            pks.size(),
            in.expected_pks.size());
    }

    const auto plan = it::explain_text<true>(*in.conn, sql);
    out.custom_scan_taken = it::plan_contains(plan, "Custom Scan (BioseqBatchSortScan)") ? 1 : 0;
    const auto accelerator = it::find_explain_property(plan, "Bioseq Sort Accelerator");
    if (accelerator.has_value()) {
        out.accelerator_gpu = (*accelerator == "gpu") ? 1 : 0;
        out.accelerator_stdc_compare = (*accelerator == "stdc_compare") ? 1 : 0;
    }
    out.gpu_sort_dispatches =
        sort_scan_explain_int(plan, "Bioseq GPU Sort Dispatches", 0);
    out.gpu_global_compare_dispatches =
        sort_scan_explain_int(plan, "Bioseq GPU Global Compare Dispatches", 0);
    out.gpu_dense_scatter_dispatches =
        sort_scan_explain_int(plan, "Bioseq GPU Dense Scatter Dispatches", 0);
    out.gpu_sort_uploaded_bytes =
        sort_scan_explain_int(plan, "Bioseq GPU Sort Uploaded Bytes", 0);
    out.sort_runtime_ns =
        sort_scan_explain_int(plan, "Bioseq Sort Runtime Ns", 0);
    out.cpu_refine_rows = 0;  // The source path has no host refinement callback.
    return out;
}

[[nodiscard]] std::map<std::string, double> sort_scan_eval(
    std::string_view,
    const ParamMap&,
    const SortScanInput&,
    const SortScanOutput& out)
{
    return {
        {"row_count", static_cast<double>(out.row_count)},
        {"sorted_correct", static_cast<double>(out.sorted_correct)},
        {"custom_scan_taken", static_cast<double>(out.custom_scan_taken)},
        {"accelerator_gpu", static_cast<double>(out.accelerator_gpu)},
        {"accelerator_stdc_compare", static_cast<double>(out.accelerator_stdc_compare)},
        {"gpu_sort_dispatches", static_cast<double>(out.gpu_sort_dispatches)},
        {"gpu_global_compare_dispatches", static_cast<double>(out.gpu_global_compare_dispatches)},
        {"gpu_dense_scatter_dispatches", static_cast<double>(out.gpu_dense_scatter_dispatches)},
        {"gpu_sort_uploaded_bytes", static_cast<double>(out.gpu_sort_uploaded_bytes)},
        {"cpu_refine_rows", static_cast<double>(out.cpu_refine_rows)},
        {"sort_runtime_ns", static_cast<double>(out.sort_runtime_ns)},
        {"measured_region_ns_manual", static_cast<double>(out.measured_region_ns)},
    };
}

}  // anonymous namespace
HWY_AFTER_NAMESPACE();    // close the Highway per-target namespace

#ifndef BIOSEQ_E2E_CONTRACT_TEST
int main(int argc, char** argv) {
    log_configure(true); fmtlog::setLogLevel(fmtlog::INF);
    const char* cfg_override = std::getenv("BIOSEQ_E2E_CONFIG_OVERRIDE");
    const std::string cfg{cfg_override != nullptr && *cfg_override != '\0'
        ? cfg_override
        : BIOSEQ_E2E_CONFIG_PATH};
    const std::string rep{BIOSEQ_E2E_REPORT_DIR}, data{BIOSEQ_E2E_DATA_DIR};
    using bioseq::bench_framework::RunConstraints; using bioseq::bench_framework::ParamMap;
    auto est = std::function<RunConstraints(std::string_view, std::string_view, simdjson::dom::element)>{e2e_estimate};
    auto inp = std::function<E2eInput(std::string_view, std::string_view, simdjson::dom::element, const RunConstraints&)>{e2e_input};
    auto run = std::function<E2eOutput(std::string_view, const ParamMap&, const E2eInput&)>{e2e_run};
    auto evl = std::function<std::map<std::string,double>(std::string_view, const ParamMap&, const E2eInput&, const E2eOutput&)>{e2e_eval};
    auto cln = std::function<void(std::string_view, std::string_view, simdjson::dom::element, const E2eInput&)>{cleanup_e2e_input};
    auto raw = std::function<std::string(std::string_view, const ParamMap&, const E2eInput&, const E2eOutput&)>{serialize_e2e_raw_sample};
    auto routing_est = std::function<RunConstraints(std::string_view, std::string_view, simdjson::dom::element)>{routing_poc_estimate};
    auto routing_inp = std::function<RoutingPocInput(std::string_view, std::string_view, simdjson::dom::element, const RunConstraints&)>{routing_poc_input};
    auto routing_run = std::function<RoutingPocOutput(std::string_view, const ParamMap&, const RoutingPocInput&)>{routing_poc_run};
    auto routing_evl = std::function<std::map<std::string,double>(std::string_view, const ParamMap&, const RoutingPocInput&, const RoutingPocOutput&)>{routing_poc_eval};
    auto routing_cln = std::function<void(std::string_view, std::string_view, simdjson::dom::element, const RoutingPocInput&)>{cleanup_routing_poc_input};
    auto sort_est = std::function<RunConstraints(std::string_view, std::string_view, simdjson::dom::element)>{sort_scan_estimate};
    auto sort_inp = std::function<SortScanInput(std::string_view, std::string_view, simdjson::dom::element, const RunConstraints&)>{sort_scan_input};
    auto sort_run = std::function<SortScanOutput(std::string_view, const ParamMap&, const SortScanInput&)>{sort_scan_run};
    auto sort_evl = std::function<std::map<std::string,double>(std::string_view, const ParamMap&, const SortScanInput&, const SortScanOutput&)>{sort_scan_eval};
    auto sort_cln = std::function<void(std::string_view, std::string_view, simdjson::dom::element, const SortScanInput&)>{cleanup_sort_scan_input};
    return bioseq::bench_framework::run(argc, argv, rep, data,
        bioseq::bench_framework::create_benchmark<E2eInput, E2eOutput>(
            "End-to-end SQL bioseq.load + mmsearch vs minimap2 (2/4 Gbp dna4/dna5)",
            "bench_e2e", cfg, est, inp, run, evl, cln, raw),
        bioseq::bench_framework::create_benchmark<E2eInput, E2eOutput>(
            "Five-repeat sign-off SQL bioseq.load + mmsearch vs minimap2 (2/4 Gbp dna4/dna5)",
            "bench_e2e_signoff", cfg, est, inp, run, evl, cln, raw),
        bioseq::bench_framework::create_benchmark<E2eInput, E2eOutput>(
            "E2E resident GPU batch proof vs SIMD and minimap2",
            "bench_e2e_gpu_batch_proof", cfg, est, inp, run, evl, cln, raw),
        bioseq::bench_framework::create_benchmark<RoutingPocInput, RoutingPocOutput>(
            "Cold persisted 8-Mbp segment routing POC",
            "bench_e2e_routing_poc", cfg,
            routing_est, routing_inp, routing_run, routing_evl, routing_cln),
        bioseq::bench_framework::create_benchmark<SortScanInput, SortScanOutput>(
            "SQL ORDER BY bioseq.seq sort-scan GPU vs stdc_compare",
            "bench_sort_scan", cfg, sort_est, sort_inp, sort_run, sort_evl, sort_cln));
}
#endif
