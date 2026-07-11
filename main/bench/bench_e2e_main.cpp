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
#include <concepts>
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
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
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
    Simd,
    Gpu,
};

struct E2eCliSelection {
    bool saw_cases_filter = false;
    bool simd_just_index = true;
    bool gpu_just_index = true;
    bool minimap2_just_index = true;
};

E2eCliSelection g_e2e_cli_selection{};

struct E2eQueryTruth {
    std::int32_t id = 0;
    std::string bases;
    std::int64_t truth_target_start = 0;
    bool truth_is_reverse = false;
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
    std::string just_index_table_gpu_per_fragment;
    std::string just_index_table_gpu_per_sequence;
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
    static constexpr std::size_t kAlignmentSnapshotColumnCount = 142;
    static constexpr std::size_t kAlignmentSnapshotRowCount = 20;
    bool found = false;
    std::int64_t target_start = -1;
    bool is_reverse = false;
    std::int64_t match_count = 0;
    std::int64_t expected_match_count = 0;
    std::int64_t correct_match_count = 0;
    std::int64_t cigar_result_count = 0;
    std::int64_t correct_cigar_count = 0;
    double abs_offset_bp = 0.0;
    double max_abs_offset_bp = 0.0;
    std::string requested_backend;
    std::string actual_backend;
    std::string requested_target_index_source;
    std::string actual_target_index_source;
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
    std::string pre_restart_persisted_source_identity;
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

void note_e2e_filtered_case(std::string_view name, E2eCliSelection& selection) {
    if (name == "bioseq_just_index"
        || name == "bioseq_block_simd_dp_two_stack_nthash_just_index") {
        selection.simd_just_index = true;
    } else if (name == "bioseq_block_vulkan_phase_c_frontend_just_index"
               || name == "bioseq_block_gpu_dp_gpu_seed_just_index") {
        selection.gpu_just_index = true;
    } else if (name == "minimap2_just_index") {
        selection.minimap2_just_index = true;
    }
}

void parse_e2e_cli_selection(int argc, char** argv) {
    E2eCliSelection selection{};
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string_view{argv[i]} != "--filter") {
            continue;
        }
        const std::string_view spec{argv[i + 1]};
        constexpr std::string_view prefix = "cases=";
        if (!spec.starts_with(prefix)) {
            continue;
        }
        selection.saw_cases_filter = true;
        selection.simd_just_index = false;
        selection.gpu_just_index = false;
        selection.minimap2_just_index = false;
        std::string_view rest = spec.substr(prefix.size());
        while (!rest.empty()) {
            const std::size_t comma = rest.find(',');
            const std::string_view item =
                rest.substr(0, comma == std::string_view::npos ? rest.size() : comma);
            note_e2e_filtered_case(item, selection);
            if (comma == std::string_view::npos) {
                break;
            }
            rest.remove_prefix(comma + 1);
        }
    }
    g_e2e_cli_selection = selection;
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

[[nodiscard]] bool is_valid_benchmark_cigar(std::string_view cigar) {
    bool saw_digit = false;
    bool saw_op = false;
    bool in_number = false;
    for (char ch : cigar) {
        if (ch >= '0' && ch <= '9') {
            saw_digit = true;
            in_number = true;
            continue;
        }
        const bool is_op = ch == 'M' || ch == 'I' || ch == 'D'
            || ch == '=' || ch == 'X' || ch == 'S' || ch == 'H';
        if (!is_op || !in_number) {
            return false;
        }
        saw_op = true;
        in_number = false;
    }
    return saw_digit && saw_op && !in_number;
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
    if (rows.empty()) { out.found = false; return; }
    const it::row_t& r0 = rows.front();                          // [header, tstart, rev]
    if (r0.size() < 3 || !r0[1].has_value()) { out.found = false; return; }
    const std::string& ts = *r0[1];
    std::int64_t v = 0;
    if (std::from_chars(ts.data(), ts.data() + ts.size(), v).ec != std::errc{}) {
        out.found = false; return;                              // malformed tstart
    }
    out.found         = true;
    out.target_start  = v;
    out.is_reverse    = r0[2].has_value() && (*r0[2] == "t" || *r0[2] == "true");
    out.abs_offset_bp = static_cast<double>(
        std::llabs(out.target_start - in.truth_target_start));
    out.max_abs_offset_bp = out.abs_offset_bp;
    out.correct_match_count =
        out.abs_offset_bp < static_cast<double>(kPositionToleranceBp) ? 1 : 0;
    if (in.cigar_projection && r0.size() >= 5
        && r0[3].has_value() && r0[4].has_value()
        && (*r0[3] == "t" || *r0[3] == "true")
        && is_expected_benchmark_cigar(*r0[4], in)) {
        out.cigar_result_count = 1;
        out.correct_cigar_count = 1;
    }
}

void parse_bioseq_batch_rows(
        const it::rowset_t& rows,
        const E2eInput& in,
        E2eOutput& out)
{
    std::vector<std::uint8_t> found(in.queries.size(), 0);
    std::vector<std::uint8_t> cigar_found(in.queries.size(), 0);
    std::vector<double> best_offset(in.queries.size(), kNoMatchOffsetSentinel);

    for (const it::row_t& row : rows) {
        if (row.size() < 4 || !row[0].has_value() || !row[2].has_value()) {
            continue;
        }
        const std::int64_t qid64 = parse_i64(*row[0]);
        if (qid64 < 0 || static_cast<std::size_t>(qid64) >= in.queries.size()) {
            continue;
        }
        const auto qid = static_cast<std::size_t>(qid64);
        const std::int64_t target_start = parse_i64(*row[2]);
        const double offset = static_cast<double>(
            std::llabs(target_start - in.queries[qid].truth_target_start));
        best_offset[qid] = std::min(best_offset[qid], offset);
        found[qid] = 1;
        const bool coordinate_correct =
            offset < static_cast<double>(kPositionToleranceBp);
        if (in.cigar_projection && coordinate_correct && row.size() >= 6
            && row[4].has_value() && row[5].has_value()
            && (row[4].value() == "t" || row[4].value() == "true")
            && is_valid_benchmark_cigar(row[5].value())) {
            ++out.cigar_result_count;
            if (is_expected_benchmark_cigar(row[5].value(), in)) {
                cigar_found[qid] = 1;
            }
        }
    }

    out.expected_match_count = static_cast<std::int64_t>(in.queries.size());
    out.match_count = static_cast<std::int64_t>(rows.size());
    out.correct_match_count = 0;
    out.correct_cigar_count = 0;
    out.max_abs_offset_bp = 0.0;
    for (std::size_t i = 0; i < in.queries.size(); ++i) {
        if (found[i] && best_offset[i] < static_cast<double>(kPositionToleranceBp)) {
            ++out.correct_match_count;
        }
        if (cigar_found[i]) {
            ++out.correct_cigar_count;
        }
        out.max_abs_offset_bp = std::max(out.max_abs_offset_bp, best_offset[i]);
    }
    out.found = out.correct_match_count == out.expected_match_count
        && (!in.cigar_projection
            || out.correct_cigar_count == out.expected_match_count);
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
};

[[nodiscard]] bool fixed_lower_hex(
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

void capture_alignment_statement_stats(
        const it::connection_test_context& connection,
        const AlignmentSnapshotIdentity& prior,
        E2eOutput& out) {
    constexpr std::size_t kColumnCount = 142;
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
        "SELECT * FROM bioseq.last_alignment_execution_stats();");
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
                || alignment_stats_text_column(column)) {
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

// minimap2 PAF: tab-separated; col indices (0-based): 0 query name, 4 strand(+/-),
// 5 target name (refN), 7 target start (per-record, forward strand), 8 target end.
// Primary = the line with tag tp:A:P; absolute target start =
// record_index*minimap2_split_bp + per_record_start. match_count counts PRIMARY
// alignments only (MEDIUM-04).
void parse_minimap2_paf(const std::string& paf, const E2eInput& in, E2eOutput& out) {
    struct BestHit {
        std::int64_t abs_start = -1;
        std::int64_t span = -1;
        bool rev = false;
    };
    auto query_id = [](std::string_view qname) -> std::int64_t {
        constexpr std::string_view prefix = "query";
        if (!qname.starts_with(prefix)) {
            return 0;
        }
        return parse_i64(qname.substr(prefix.size()));
    };
    std::vector<BestHit> primary_best(in.queries.size());
    std::vector<BestHit> any_best(in.queries.size());
    std::int64_t primaries = 0;
    std::size_t pos = 0;
    while (pos < paf.size()) {
        const std::size_t eol = paf.find('\n', pos);
        const std::string_view line{paf.data() + pos, (eol == std::string::npos ? paf.size() : eol) - pos};
        pos = (eol == std::string::npos) ? paf.size() : eol + 1;
        if (line.empty()) continue;
        std::vector<std::string_view> col; col.reserve(18);
        std::size_t cs = 0;
        for (std::size_t i = 0; i <= line.size(); ++i)
            if (i == line.size() || line[i] == '\t') { col.emplace_back(line.substr(cs, i - cs)); cs = i + 1; }
        if (col.size() < 9) continue;
        const std::int64_t qid64 = query_id(col[0]);
        if (qid64 < 0 || static_cast<std::size_t>(qid64) >= in.queries.size()) {
            continue;
        }
        const auto qid = static_cast<std::size_t>(qid64);
        const std::string_view rname = col[5];                 // "refN"
        const std::int64_t rec_idx = (rname.size() > 3) ? parse_i64(rname.substr(3)) : 0;
        const std::int64_t rstart  = parse_i64(col[7]);
        const std::int64_t abs_start =
            rec_idx * static_cast<std::int64_t>(in.minimap2_split_bp) + rstart;
        const std::int64_t span = parse_i64(col[8]) - rstart;  // target end - start
        const bool rev = (col[4] == "-");
        const bool is_primary = (line.find("tp:A:P") != std::string_view::npos);
        if (is_primary) ++primaries;
        if (span > any_best[qid].span) {
            any_best[qid] = BestHit{abs_start, span, rev};
        }
        if (is_primary && span > primary_best[qid].span) {
            primary_best[qid] = BestHit{abs_start, span, rev};
        }
    }

    out.expected_match_count = static_cast<std::int64_t>(in.queries.size());
    out.match_count = primaries;
    out.correct_match_count = 0;
    out.max_abs_offset_bp = 0.0;
    for (std::size_t i = 0; i < in.queries.size(); ++i) {
        const BestHit& best =
            primary_best[i].abs_start >= 0 ? primary_best[i] : any_best[i];
        if (i == 0) {
            out.target_start = best.abs_start;
            out.is_reverse = best.rev;
        }
        const double offset = best.abs_start >= 0
            ? static_cast<double>(
                std::llabs(best.abs_start - in.queries[i].truth_target_start))
            : kNoMatchOffsetSentinel;
        out.max_abs_offset_bp = std::max(out.max_abs_offset_bp, offset);
        if (offset < static_cast<double>(kPositionToleranceBp)) {
            ++out.correct_match_count;
        }
    }
    out.found = out.correct_match_count == out.expected_match_count;
    out.abs_offset_bp = out.max_abs_offset_bp;
}

// HIGH-09/TESTING-04: the ground truth is known, so any miss or mis-map is ALWAYS
// a bug. Fail loudly rather than letting a fast, meaningless timing be reported
// as a "win". Called AFTER the timed region.
inline void require_mapped(std::string_view algo, const E2eInput& in,
                           const E2eOutput& out) {
    const bool minimap2 = algo == "minimap2_no_index"
        || algo == "minimap2_just_index";
    const bool enough_rows =
        out.expected_match_count > 0
        && out.match_count >= out.expected_match_count;
    const bool all_expected_correct =
        out.expected_match_count > 0
        && out.correct_match_count == out.expected_match_count;
    const bool cigar_ok =
        !in.cigar_projection
        || minimap2
        || out.correct_cigar_count == out.expected_match_count;
    if (out.found
        && enough_rows
        && all_expected_correct
        && cigar_ok
        && out.abs_offset_bp < static_cast<double>(kPositionToleranceBp)) {
        return;
    }
    bioseq_log<ERR>("[bench_e2e] {} failed to map the query at {} bp (found={}, "
         "target_start={}, truth={}, offset={} bp >= {} threshold) — the timed result is "
         "invalid; aborting (match_count={}, expected={}, correct={}, "
         "correct_cigar={}, cigar_projection={})",
         algo, in.target_bp, out.found, out.target_start,
         in.truth_target_start, out.abs_offset_bp, kPositionToleranceBp,
         out.match_count, out.expected_match_count, out.correct_match_count,
         out.correct_cigar_count, in.cigar_projection);
    // fmtlog is async; the ERR above can be lost when std::abort() runs before
    // the background poll flushes it. Emit a synchronous copy so the failing
    // cell is always visible in the captured stderr.
    std::fprintf(stderr,
        "[bench_e2e] MAP-FAIL algo=%.*s target_bp=%llu found=%d target_start=%lld "
        "truth=%lld offset_bp=%.1f tol=%lld match_count=%lld expected=%lld "
        "correct=%lld correct_cigar=%lld cigar_projection=%d — aborting\n",
        static_cast<int>(algo.size()), algo.data(),
        static_cast<unsigned long long>(in.target_bp), static_cast<int>(out.found),
        static_cast<long long>(out.target_start),
        static_cast<long long>(in.truth_target_start),
        out.abs_offset_bp, static_cast<long long>(kPositionToleranceBp),
        static_cast<long long>(out.match_count),
        static_cast<long long>(out.expected_match_count),
        static_cast<long long>(out.correct_match_count),
        static_cast<long long>(out.correct_cigar_count),
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
        const PipelineProofExpectation& expectation) {
    const bool expect_gpu =
        algo.find("_gpu_") != std::string_view::npos
        || algo.find("_vulkan_") != std::string_view::npos;
    const bool expect_simd =
        algo.find("_simd_") != std::string_view::npos;
    if (!expect_gpu && !expect_simd) {
        return;
    }
    const bool expect_persisted =
        algo.ends_with("_just_index");
    const std::string_view expected_backend =
        expect_gpu ? "vulkan_gpu" : "simd_cpu";
    const std::string_view expected_source =
        expect_persisted ? "persisted" : "recomputed";
    const auto expected_backend_enum = expect_gpu
        ? bioseq::pipeline_stats::ExecutedBackend::vulkan_gpu
        : bioseq::pipeline_stats::ExecutedBackend::simd_cpu;
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
    const bool persisted_read_gate = expect_persisted
        ? out.persisted_generation_read_sessions == 1
            && out.persisted_generation_read_sessions == out.target_rows
            && out.persisted_relation_opens == out.target_rows
            && counter(51) == out.persisted_generation_read_sessions
            && counter(52) == out.persisted_generation_read_sessions
            && scaled_equal(counter(53), counter(52), 64)
            && counter(54) == out.persisted_generation_read_sessions
            && counter(55) == out.persisted_generation_read_sessions
            && scaled_equal(counter(56), counter(55), 128)
            && counter(57) > 0
            && counter(58) > 0
            && counter(59) == 0
            && static_cast<__int128>(counter(60))
                == static_cast<__int128>(counter(52))
                    + static_cast<__int128>(counter(55))
                    + static_cast<__int128>(counter(57))
            && static_cast<__int128>(counter(61))
                == static_cast<__int128>(counter(53))
                    + static_cast<__int128>(counter(56))
                    + static_cast<__int128>(counter(58))
            && counter(63) > 0
            && counter(67) > 0
            && counter(68) > 0
            && counter(68) == counter(69)
            && counter(70) > 0
            && counter(70) == counter(74)
            && counter(71) > 0
            && counter(71) == counter(75)
            && counter(76) > 0
            && counter(77) == out.target_rows
            && counter(78) > 0
            && counter(78) == counter(79)
            && counter(80) == counter(79)
            && static_cast<__int128>(out.persisted_index_scans)
                == static_cast<__int128>(counter(51))
                    + static_cast<__int128>(counter(54))
                    + static_cast<__int128>(counter(80))
            && sum_equal(counter(62), counter(75), counter(72))
            && sum_equal(counter(64), counter(60), counter(62))
            && sum_equal(counter(76), counter(61), counter(63))
            && counter(72) <= counter(81)
            && counter(65) == 0
            && counter(66) == 0
            && counter(73) == 0
            && counter(82) == 0
            && counter(84) == 0
            && counter(85) == 0
        : std::all_of(
            out.alignment_snapshot_numeric_columns.begin() + 48,
            out.alignment_snapshot_numeric_columns.begin() + 84,
            [](const std::int64_t value) noexcept { return value == 0; });
    const bool cpu_prepared_gate = expect_persisted && !expect_gpu
        ? out.cpu_prepared_index_builds == 1
            && out.cpu_prepared_index_reuses
                == expectation.attempted_alignment_jobs - 1
            && out.cpu_prepared_unique_keys > 0
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
    const bool simd_operational_gate = !expect_simd
        || (counter(93) == 0
            && counter(94) > 0
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
    const bool gpu_operational_gate = !expect_gpu
        || (counter(82) == 0
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
            && counter(112) == 0
            && counter(124) == 1
            && counter(125) == out.gpu_pipeline_cache_seed_size
            && out.gpu_pipeline_cache_hit == 1
            && counter(133) == 0);
    const bool cold_restart_gate = !expect_persisted
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
                == out.persisted_generation_id
            && fixed_lower_hex(out.pre_restart_persisted_source_identity, 16)
            && out.pre_restart_persisted_source_identity
                != "0000000000000000"
            && out.pre_restart_persisted_source_identity
                == out.persisted_source_identity);
    const bool cold_no_index_gate = expect_persisted
        || out.cold_no_index_lifecycle_accepted == 1;
    const bool accepted =
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
        && expectation.attempted_alignment_jobs > 0
        && exact_cardinality
        && gpu_signoff_shape
        && out.fully_attested_alignment_jobs
            == out.attempted_alignment_jobs
        && out.unattested_alignment_jobs == 0
        && out.requested_backend == expected_backend
        && out.actual_backend == expected_backend
        && out.requested_target_index_source == expected_source
        && out.actual_target_index_source == expected_source
        && out.environment_mixed_invalid == 0
        && out.mixed_persisted_identity == 0
        && out.operational_identity_conflict == 0
        && out.stats_saturated == 0
        && out.backend_declines == 0
        && out.backend_fallbacks == 0
        && row_equation
        && source_valid
        && persisted_identity_valid
        && persisted_read_gate
        && cpu_prepared_gate
        && gpu_residency_gate
        && simd_operational_gate
        && gpu_operational_gate
        && cold_restart_gate
        && cold_no_index_gate
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
        static_cast<long long>(out.backend_declines),
        static_cast<long long>(out.backend_fallbacks));
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
    const bool use_gpu = setup_kind == JustIndexSetupKind::Gpu;
    if (minimizer_index == kMinimizersPerSequenceIndex) {
        return use_gpu
            ? in.just_index_table_gpu_per_sequence
            : in.just_index_table_simd_per_sequence;
    }
    return use_gpu
        ? in.just_index_table_gpu_per_fragment
        : in.just_index_table_simd_per_fragment;
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
                       simdjson::dom::element,
                       const E2eInput& in) {
    const char* keep_artifacts_env = std::getenv("BIOSEQ_E2E_KEEP_ARTIFACTS");
    if (keep_artifacts_env != nullptr
        && std::string_view{keep_artifacts_env} == "1") {
        bioseq_log<DBG>("[bench_e2e] keeping SQL tables and FASTA/MMI artifacts "
                "because BIOSEQ_E2E_KEEP_ARTIFACTS=1");
        return;
    }

    if (in.conn != nullptr) {
        it::exec_sqlf(*in.conn, "DROP TABLE IF EXISTS {} CASCADE;",
                      in.just_index_table_simd_per_fragment);
        it::exec_sqlf(*in.conn, "DROP TABLE IF EXISTS {} CASCADE;",
                      in.just_index_table_simd_per_sequence);
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
                                       const RunConstraints&) {
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
    in.just_index_table_gpu_per_fragment =
        fmt::format("e2e_{}_just_gpu_mpf", in.size_label);
    in.just_index_table_gpu_per_sequence =
        fmt::format("e2e_{}_just_gpu_mps", in.size_label);
    in.just_index_table_per_fragment = in.just_index_table_simd_per_fragment;
    in.just_index_table_per_sequence = in.just_index_table_simd_per_sequence;
    in.just_index_table = in.just_index_table_per_sequence;
    in.no_index_table = fmt::format("e2e_{}_noidx", in.size_label);

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
            .truth_is_reverse = pair.is_reverse,
        });
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

    // (2) write FASTA via the bounded IO sequence_writer over zero-copy views.
    write_single_record_fasta<A>(in.target_fasta, "ref", *first.target);
    write_split_fasta<A>(in.target_fasta_split, "ref", *first.target, split_bp);
    in.query_bases = in.queries.front().bases;   // compatibility literal path
    write_multi_query_fasta<A>(in.query_fasta, in.queries);

    // (3) connect once; clean slate.
    in.conn = std::make_shared<it::connection_test_context>();
    it::reset_extension(*in.conn, { std::string_view{in.just_index_table_simd_per_fragment},
                                    std::string_view{in.just_index_table_simd_per_sequence},
                                    std::string_view{in.just_index_table_gpu_per_fragment},
                                    std::string_view{in.just_index_table_gpu_per_sequence},
                                    std::string_view{in.no_index_table} });
    preflight_e2e_resources(*in.conn, in.size_label, in.target_bp);

    // (4) force the fastest bioseq combo for the whole session.
    configure_bioseq_simd_session(*in.conn, in.kmer_size, in.window_size);

    const char* skip_index_setup_env =
        std::getenv("BIOSEQ_E2E_SKIP_INDEX_BASELINE_SETUP");
    const bool skip_index_baseline_setup =
        jbool(data, "skip_index_baseline_setup", false)
        || (skip_index_setup_env != nullptr
            && std::string_view{skip_index_setup_env} == "1");
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
    const auto should_setup_just_index =
        [&just_index_setup_only_name](std::string_view index_name) {
            return just_index_setup_only_name.empty()
                || just_index_setup_only_name == index_name;
        };
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
            } else {
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
            }
            configure_bioseq_minimizer_policy(*in.conn, table, index_name);
            it::exec_sql(*in.conn, "SET bioseq.update_index_on_insert = on;");
            it::exec_sql(*in.conn, "SET bioseq.read_sequence_index = on;");
            it::exec_sqlf(
                *in.conn,
                "SELECT bioseq.load('{}'::regclass, '{}');",
                table,
                in.target_fasta);
            const std::string audit_sql = fmt::format(
                "SELECT current_setting('bioseq.aligner.minimizer_algorithm'), "
                "current_setting('bioseq.aligner.vulkan_enabled'), "
                "current_setting('bioseq.aligner.vulkan_gpu_index_build_enabled'), "
                "bioseq.index_table_row_count('{}'::regclass, '{}')",
                table,
                index_name);
            const auto audit_rows = it::query_rows(*in.conn, audit_sql);
            const std::int64_t setup_rows =
                audit_rows.empty() || audit_rows.front().size() < 4
                    || !audit_rows.front()[3].has_value()
                    ? -1
                    : parse_i64(*audit_rows.front()[3]);
            if (audit_rows.empty() || setup_rows <= 0) {
                bioseq_log<ERR>(
                    "[bench_e2e] invalid just_index setup audit for table={} index={} rows={}",
                    table,
                    index_name,
                    setup_rows);
                std::abort();
            }
        };

    // (5) just_index tables: each indexed cell gets its own table and policy.
    // This avoids a per-sequence run accidentally succeeding through a residual
    // per-fragment index, and keeps the per-fragment cell valid too.
    if (!skip_index_baseline_setup
        && (g_e2e_cli_selection.simd_just_index
            || g_e2e_cli_selection.gpu_just_index)) {
        if (should_setup_just_index(kMinimizersPerFragmentIndex)) {
            if (g_e2e_cli_selection.simd_just_index) {
                setup_just_index_table(
                    in.just_index_table_simd_per_fragment,
                    kMinimizersPerFragmentIndex,
                    JustIndexSetupKind::Simd);
            }
            if (g_e2e_cli_selection.gpu_just_index) {
                setup_just_index_table(
                    in.just_index_table_gpu_per_fragment,
                    kMinimizersPerFragmentIndex,
                    JustIndexSetupKind::Gpu);
            }
        }
        if (should_setup_just_index(kMinimizersPerSequenceIndex)) {
            if (g_e2e_cli_selection.simd_just_index) {
                setup_just_index_table(
                    in.just_index_table_simd_per_sequence,
                    kMinimizersPerSequenceIndex,
                    JustIndexSetupKind::Simd);
            }
            if (g_e2e_cli_selection.gpu_just_index) {
                setup_just_index_table(
                    in.just_index_table_gpu_per_sequence,
                    kMinimizersPerSequenceIndex,
                    JustIndexSetupKind::Gpu);
            }
        }
    } else {
        bioseq_log<DBG>("[bench_e2e] skipping bioseq persisted-index setup "
                "(skip_index_baseline_setup={}, simd_just_index={}, gpu_just_index={})",
                skip_index_baseline_setup,
                g_e2e_cli_selection.simd_just_index,
                g_e2e_cli_selection.gpu_just_index);
    }

    // (6) no_index table: create empty. Each rep loads base data untimed, proves
    // both minimizer sidecars absent, restarts PostgreSQL, then times only
    // transient index construction + mapping.
    create_e2e_table(*in.conn, in.no_index_table, in.seq_type);

    // (7) minimap2 prebuilt .mmi for the just_index baseline (untimed).
    if (!skip_index_baseline_setup && !skip_minimap2_baseline_setup
        && g_e2e_cli_selection.minimap2_just_index) {
        run_cmd(fmt::format("{} -k{} -w{} -d {} {}", BIOSEQ_E2E_MINIMAP2_BIN,
                            in.kmer_size, in.window_size, in.mmi_path, in.target_fasta_split));
    } else if (skip_minimap2_baseline_setup) {
        bioseq_log<DBG>("[bench_e2e] skipping minimap2 .mmi baseline setup because "
                "BIOSEQ_E2E_SKIP_MINIMAP2_BASELINE_SETUP=1");
    } else if (!g_e2e_cli_selection.minimap2_just_index) {
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

[[nodiscard]] std::string e2e_projection_sql(const E2eInput& in) {
    if (!in.cigar_projection) {
        return "bioseq.target_start(header) AS tstart, "
               "bioseq.is_reverse(header) AS rev";
    }
    return "bioseq.target_start(header) AS tstart, "
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

    if (algo == "bioseq_just_index"
        || algo == "bioseq_block_simd_dp_two_stack_nthash_just_index") {
        // Restart PostgreSQL first so the prebuilt index (built untimed in
        // on_input) is assembled cold from storage rather than served warm.
        out.just_index_restart_performed =
            restart_postgres_for_cold_just_index(in, out) ? 1 : 0;
        configure_bioseq_scheduler_session(*in.conn, pipeline_mode);
        configure_bioseq_pair_sort_session(*in.conn, params);
        configure_bioseq_simd_session(*in.conn, in.kmer_size, in.window_size);
        const std::string& table =
            just_index_table_for(in, minimizer_index, JustIndexSetupKind::Simd);
        configure_bioseq_minimizer_policy(
            *in.conn, table, minimizer_index);
        it::exec_sql(*in.conn, "SET bioseq.read_sequence_index = on;");
        load_e2e_query_table(*in.conn, in);
        const std::string sql = batched_coord_sql(table, proj, 2);
        const AlignmentSnapshotIdentity prior_alignment =
            capture_prior_alignment_identity(*in.conn);
        ScopedResourceMonitor resources;
        const auto start_ns = static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
        {
            auto _scope = bioseq::instr::region(kMeasuredRegion); // TIMED: assemble only
            parse_bioseq_batch_rows(it::query_rows(*in.conn, sql), in, out);
        }
        const auto end_ns = static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
        attach_resource_metrics(out, start_ns, end_ns, resources.stop());
        capture_alignment_statement_stats(
            *in.conn, prior_alignment, out);
        require_pipeline_execution_proof(
            algo, in, out,
            pipeline_proof_expectation(in, true, fair_workers));
        require_mapped(algo, in, out);
        capture_mps_build_stats(*in.conn, out);
        capture_index_audit(*in.conn, table, minimizer_index, out);
        log_measured_case(
            algo, pipeline_mode, minimap_threads, in, out, minimizer_index);

    } else if (algo == "bioseq_block_vulkan_phase_c_frontend_just_index"
               || algo == "bioseq_block_gpu_dp_gpu_seed_just_index") {
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
        const std::string& table =
            just_index_table_for(in, minimizer_index, JustIndexSetupKind::Gpu);
        configure_bioseq_minimizer_policy(*in.conn, table, minimizer_index);
        it::exec_sql(*in.conn, "SET bioseq.read_sequence_index = on;");
        configure_bioseq_vulkan_index_session(
            *in.conn,
            in.kmer_size,
            in.window_size,
            in.cigar_projection);  // DOC0345 Patch L
        load_e2e_query_table(*in.conn, in);
        const std::string sql = batched_coord_sql(table, proj, 2);
        const AlignmentSnapshotIdentity prior_alignment =
            capture_prior_alignment_identity(*in.conn);
        ScopedResourceMonitor resources;
        const auto start_ns = static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
        {
            auto _scope = bioseq::instr::region(kMeasuredRegion); // TIMED: assemble only
            parse_bioseq_batch_rows(it::query_rows(*in.conn, sql), in, out);
        }
        const auto end_ns = static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
        attach_resource_metrics(out, start_ns, end_ns, resources.stop());
        capture_alignment_statement_stats(
            *in.conn, prior_alignment, out);
        require_pipeline_execution_proof(
            algo, in, out,
            pipeline_proof_expectation(in, true, fair_workers));
        require_mapped(algo, in, out);
        capture_mps_build_stats(*in.conn, out);
        capture_index_audit(*in.conn, table, minimizer_index, out);
        log_measured_case(algo, pipeline_mode, minimap_threads, in, out, minimizer_index);

    } else if (algo == "bioseq_no_index"
               || algo == "bioseq_block_simd_dp_two_stack_nthash_no_index") {
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
        const std::string align_sql =
            batched_coord_sql(in.no_index_table, proj, 1);
        load_e2e_query_table(*in.conn, in);
        const AlignmentSnapshotIdentity prior_alignment =
            capture_prior_alignment_identity(*in.conn);
        ScopedResourceMonitor resources;
        const auto start_ns = static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
        {
            auto _scope = bioseq::instr::region(kMeasuredRegion);
            // TIMED: transient exact target-index construction + mapping only.
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
            pipeline_proof_expectation(in, false, fair_workers));
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
        load_e2e_query_table(*in.conn, in);
        const AlignmentSnapshotIdentity prior_alignment =
            capture_prior_alignment_identity(*in.conn);
        ScopedResourceMonitor resources;
        const auto start_ns = static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
        {
            auto _scope = bioseq::instr::region(kMeasuredRegion);
            // TIMED: transient exact GPU target-index construction + mapping only.
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
            pipeline_proof_expectation(in, false, fair_workers));
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
        const std::string cmd = fmt::format("{} -c -t{} -k{} -w{} {} {}",
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
        const std::string cmd = fmt::format("{} -c -t{} {} {}",
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
    return {
        {"match_count",             static_cast<double>(o.match_count)},
        {"expected_match_count",    static_cast<double>(o.expected_match_count)},
        {"correct_match_count",     static_cast<double>(o.correct_match_count)},
        {"cigar_result_count",      static_cast<double>(o.cigar_result_count)},
        {"correct_cigar_count",     static_cast<double>(o.correct_cigar_count)},
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
        "bioseq.last_alignment_execution_stats/raw-rowset";
    snapshot["schema_version"] = 1;
    snapshot["expected_row_count"] =
        E2eOutput::kAlignmentSnapshotRowCount;
    snapshot["actual_row_count"] = out.alignment_snapshot_rows.size();
    snapshot["expected_column_count"] =
        E2eOutput::kAlignmentSnapshotColumnCount;

    nlohmann::json columns = nlohmann::json::array();
    for (std::size_t column = 0;
         column < kAlignmentSnapshotColumnNames.size(); ++column) {
        const std::string_view json_type = alignment_stats_text_column(column)
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
                "alignment SRF row does not have exactly 142 columns");
        }
        nlohmann::json serialized_row = nlohmann::json::array();
        for (std::size_t column = 0; column < row.size(); ++column) {
            const auto& cell = row[column];
            if (!cell.has_value()) {
                serialized_row.push_back(nullptr);
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
    const bool attested_accelerator_sample =
        algorithm.find("_simd_") != std::string_view::npos
        || algorithm.find("_gpu_") != std::string_view::npos
        || algorithm.find("_vulkan_") != std::string_view::npos;
    if (postgres_sample
        && out.alignment_snapshot_rows.size()
            != E2eOutput::kAlignmentSnapshotRowCount) {
        abort_raw_evidence_serialization(
            "bioseq sample has no complete same-invocation SRF evidence");
    }
    if (attested_accelerator_sample
        && out.pipeline_proof_accepted != 1) {
        abort_raw_evidence_serialization(
            "accelerator sample was not accepted before serialization");
    }
    if (postgres_sample && algorithm.ends_with("_no_index")
        && out.cold_no_index_lifecycle_accepted != 1) {
        abort_raw_evidence_serialization(
            "no_index sample lacks accepted cold lifecycle evidence");
    }

    nlohmann::json payload = nlohmann::json::object();
    payload["payload_schema"] = "bioseq.e2e.raw-sample";
    payload["payload_schema_version"] = 1;
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
        {"cigar_result_count", out.cigar_result_count},
        {"correct_cigar_count", out.correct_cigar_count},
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
    } else {
        payload["postgres"] = nullptr;
        payload["alignment_snapshot"] = nullptr;
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
        {"backend_declines", out.backend_declines},
        {"backend_fallbacks", out.backend_fallbacks},
        {"stats_saturated", out.stats_saturated},
    };
    return payload.dump();
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

int main(int argc, char** argv) {
    log_configure(true); fmtlog::setLogLevel(fmtlog::INF);
    parse_e2e_cli_selection(argc, argv);
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
            "E2E resident GPU batch proof vs SIMD and minimap2",
            "bench_e2e_gpu_batch_proof", cfg, est, inp, run, evl, cln, raw),
        bioseq::bench_framework::create_benchmark<SortScanInput, SortScanOutput>(
            "SQL ORDER BY bioseq.seq sort-scan GPU vs stdc_compare",
            "bench_sort_scan", cfg, sort_est, sort_inp, sort_run, sort_evl, sort_cln));
}
