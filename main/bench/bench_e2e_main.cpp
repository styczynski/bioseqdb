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
constexpr std::int64_t  kPositionToleranceBp = 50;   // DOC0283: <=2 bp scanner drift; 50 generous
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
    std::string no_index_table;                  // empty; loaded inside the region
    std::string batch_pair_table;                // optional same-rel target/query proof table
    std::string target_fasta;                    // single-record, for bioseq.load
    std::string target_fasta_split;              // <=split_bp records, for minimap2
    std::string query_fasta;                     // for minimap2
    std::string mmi_path;                        // prebuilt minimap2 index
    std::string query_bases;                     // ACGTN text for the bioseq SQL literal
    std::string target_bases;                    // shared target for batch_pair_table rows
    std::vector<E2eQueryTruth> queries;
    int kmer_size = 15, window_size = 10;
    bool batch_pair_mode = false;
    bool batch_pair_load_timed = false;
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

struct E2eOutput {
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
    std::int64_t gpu_batched_branch_taken = -1;
    std::int64_t gpu_problem_count = -1;
    std::int64_t gpu_wave_count = -1;
    std::int64_t gpu_segment_indexes_built = -1;
    std::int64_t gpu_segment_indexes_reused = -1;
    std::int64_t gpu_seed_dispatches = -1;
    std::int64_t gpu_chain_dispatches = -1;
    std::int64_t gpu_extend_dispatches = -1;
    std::int64_t cpu_fallback_rows = -1;
    std::int64_t simd_fallback_rows = -1;
    std::int64_t vulkan_declines = -1;
    std::int64_t gpu_cigar_jobs = -1;
    std::int64_t gpu_cigar_bounded_jobs = -1;
    std::int64_t gpu_cigar_exact_jobs = -1;
    std::int64_t gpu_cigar_unit_gap_jobs = -1;
    std::int64_t gpu_cigar_gridded_jobs = -1;
    std::int64_t gpu_cigar_query_symbols = -1;
    std::int64_t gpu_cigar_target_symbols = -1;
    std::int64_t gpu_cigar_packed_words = -1;
    std::int64_t gpu_cigar_traceback_words = -1;
    std::int64_t gpu_cigar_prepare_ns = -1;
    std::int64_t gpu_cigar_symbol_pack_ns = -1;
    std::int64_t gpu_cigar_classify_ns = -1;
    std::int64_t gpu_cigar_runtime_ns = -1;
    std::int64_t gpu_cigar_decode_ns = -1;
    std::uint64_t measured_region_ns = 0;
    std::uint64_t peak_postgres_rss_kb = 0;
    std::uint64_t peak_minimap2_rss_kb = 0;
    std::uint64_t peak_bench_rss_kb = 0;
    std::uint64_t min_tmp_available_kb = 0;
    double peak_swap_used_mb = 0.0;
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
};

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
    out.swap_used_mb = parse_double_prefix(run_cmd_capture(
        "sysctl -n vm.swapusage | awk "
        "'{for (i=1; i<=NF; ++i) if ($i == \"used\") {v=$(i+2); "
        "gsub(\"M\", \"\", v); print v; exit}}'"));
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
        out.abs_offset_bp <= static_cast<double>(kPositionToleranceBp) ? 1 : 0;
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
        if (in.cigar_projection && row.size() >= 6
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
        if (found[i] && best_offset[i] <= static_cast<double>(kPositionToleranceBp)) {
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

[[nodiscard]] std::int64_t explain_i64_or(
        simdjson::dom::element value,
        std::int64_t fallback)
{
    std::int64_t signed_value = 0;
    if (value.get(signed_value) == simdjson::SUCCESS) {
        return signed_value;
    }
    std::uint64_t unsigned_value = 0;
    if (value.get(unsigned_value) == simdjson::SUCCESS
        && unsigned_value <= static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max())) {
        return static_cast<std::int64_t>(unsigned_value);
    }
    bool bool_value = false;
    if (value.get(bool_value) == simdjson::SUCCESS) {
        return bool_value ? 1 : 0;
    }
    return fallback;
}

void assign_gpu_explain_metric(
        std::string_view key,
        simdjson::dom::element value,
        E2eOutput& out)
{
    if (key == "Batched Branch Taken") {
        out.gpu_batched_branch_taken =
            explain_i64_or(value, out.gpu_batched_branch_taken);
    } else if (key == "GPU Problem Count") {
        out.gpu_problem_count = explain_i64_or(value, out.gpu_problem_count);
    } else if (key == "GPU Wave Count") {
        out.gpu_wave_count = explain_i64_or(value, out.gpu_wave_count);
    } else if (key == "GPU Segment Indexes Built") {
        out.gpu_segment_indexes_built =
            explain_i64_or(value, out.gpu_segment_indexes_built);
    } else if (key == "GPU Segment Indexes Reused") {
        out.gpu_segment_indexes_reused =
            explain_i64_or(value, out.gpu_segment_indexes_reused);
    } else if (key == "GPU Seed Dispatches") {
        out.gpu_seed_dispatches = explain_i64_or(value, out.gpu_seed_dispatches);
    } else if (key == "GPU Chain Dispatches") {
        out.gpu_chain_dispatches = explain_i64_or(value, out.gpu_chain_dispatches);
    } else if (key == "GPU Extend Dispatches") {
        out.gpu_extend_dispatches =
            explain_i64_or(value, out.gpu_extend_dispatches);
    } else if (key == "CPU Fallback Rows") {
        out.cpu_fallback_rows = explain_i64_or(value, out.cpu_fallback_rows);
    } else if (key == "SIMD Fallback Rows") {
        out.simd_fallback_rows = explain_i64_or(value, out.simd_fallback_rows);
    } else if (key == "Vulkan Declines") {
        out.vulkan_declines = explain_i64_or(value, out.vulkan_declines);
    } else if (key == "GPU CIGAR Jobs") {
        out.gpu_cigar_jobs = explain_i64_or(value, out.gpu_cigar_jobs);
    } else if (key == "GPU CIGAR Bounded Jobs") {
        out.gpu_cigar_bounded_jobs =
            explain_i64_or(value, out.gpu_cigar_bounded_jobs);
    } else if (key == "GPU CIGAR Exact Jobs") {
        out.gpu_cigar_exact_jobs =
            explain_i64_or(value, out.gpu_cigar_exact_jobs);
    } else if (key == "GPU CIGAR Unit Gap Jobs") {
        out.gpu_cigar_unit_gap_jobs =
            explain_i64_or(value, out.gpu_cigar_unit_gap_jobs);
    } else if (key == "GPU CIGAR Gridded Jobs") {
        out.gpu_cigar_gridded_jobs =
            explain_i64_or(value, out.gpu_cigar_gridded_jobs);
    } else if (key == "GPU CIGAR Query Symbols") {
        out.gpu_cigar_query_symbols =
            explain_i64_or(value, out.gpu_cigar_query_symbols);
    } else if (key == "GPU CIGAR Target Symbols") {
        out.gpu_cigar_target_symbols =
            explain_i64_or(value, out.gpu_cigar_target_symbols);
    } else if (key == "GPU CIGAR Packed Words") {
        out.gpu_cigar_packed_words =
            explain_i64_or(value, out.gpu_cigar_packed_words);
    } else if (key == "GPU CIGAR Traceback Words") {
        out.gpu_cigar_traceback_words =
            explain_i64_or(value, out.gpu_cigar_traceback_words);
    } else if (key == "GPU CIGAR Prepare Ns") {
        out.gpu_cigar_prepare_ns =
            explain_i64_or(value, out.gpu_cigar_prepare_ns);
    } else if (key == "GPU CIGAR Symbol Pack Ns") {
        out.gpu_cigar_symbol_pack_ns =
            explain_i64_or(value, out.gpu_cigar_symbol_pack_ns);
    } else if (key == "GPU CIGAR Classify Ns") {
        out.gpu_cigar_classify_ns =
            explain_i64_or(value, out.gpu_cigar_classify_ns);
    } else if (key == "GPU CIGAR Runtime Ns") {
        out.gpu_cigar_runtime_ns =
            explain_i64_or(value, out.gpu_cigar_runtime_ns);
    } else if (key == "GPU CIGAR Decode Ns") {
        out.gpu_cigar_decode_ns =
            explain_i64_or(value, out.gpu_cigar_decode_ns);
    }
}

void walk_gpu_explain_json(simdjson::dom::element element, E2eOutput& out)
{
    const simdjson::dom::element_type type = element.type();
    if (type == simdjson::dom::element_type::ARRAY) {
        simdjson::dom::array array_value;
        if (element.get(array_value) != simdjson::SUCCESS) {
            return;
        }
        for (simdjson::dom::element child : array_value) {
            walk_gpu_explain_json(child, out);
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
        assign_gpu_explain_metric(field.key, field.value, out);
        walk_gpu_explain_json(field.value, out);
    }
}

[[nodiscard]] std::optional<std::int64_t> gpu_batch_text_value(
        std::string_view line,
        std::string_view key)
{
    const std::size_t key_pos = line.find(key);
    if (key_pos == std::string_view::npos) {
        return std::nullopt;
    }
    std::size_t value_begin = key_pos + key.size();
    while (value_begin < line.size() && line[value_begin] == ' ') {
        ++value_begin;
    }
    std::size_t value_end = value_begin;
    while (value_end < line.size()
           && line[value_end] != ' '
           && line[value_end] != ']') {
        ++value_end;
    }
    const std::string_view token =
        line.substr(value_begin, value_end - value_begin);
    if (token == "true") {
        return 1;
    }
    if (token == "false") {
        return 0;
    }
    std::int64_t parsed = 0;
    const auto* first = token.data();
    const auto* last = token.data() + token.size();
    const auto result = std::from_chars(first, last, parsed);
    if (result.ec == std::errc{} && result.ptr == last) {
        return parsed;
    }
    return std::nullopt;
}

void assign_gpu_text_metrics(std::string_view line, E2eOutput& out)
{
    if (auto value = gpu_batch_text_value(line, "taken=")) {
        out.gpu_batched_branch_taken = *value;
    }
    if (auto value = gpu_batch_text_value(line, "problems=")) {
        out.gpu_problem_count = *value;
    }
    if (auto value = gpu_batch_text_value(line, "waves=")) {
        out.gpu_wave_count = *value;
    }
    if (auto value = gpu_batch_text_value(line, "idx_built=")) {
        out.gpu_segment_indexes_built = *value;
    }
    if (auto value = gpu_batch_text_value(line, "idx_reused=")) {
        out.gpu_segment_indexes_reused = *value;
    }
    if (auto value = gpu_batch_text_value(line, "seed_dispatches=")) {
        out.gpu_seed_dispatches = *value;
    }
    if (auto value = gpu_batch_text_value(line, "chain_dispatches=")) {
        out.gpu_chain_dispatches = *value;
    }
    if (auto value = gpu_batch_text_value(line, "extend_dispatches=")) {
        out.gpu_extend_dispatches = *value;
    }
    if (auto value = gpu_batch_text_value(line, "cpu_fallback_rows=")) {
        out.cpu_fallback_rows = *value;
    }
    if (auto value = gpu_batch_text_value(line, "simd_fallback_rows=")) {
        out.simd_fallback_rows = *value;
    }
    if (auto value = gpu_batch_text_value(line, "declines=")) {
        out.vulkan_declines = *value;
    }
    if (auto value = gpu_batch_text_value(line, "jobs=")) {
        out.gpu_cigar_jobs = *value;
    }
    if (auto value = gpu_batch_text_value(line, "bounded=")) {
        out.gpu_cigar_bounded_jobs = *value;
    }
    if (auto value = gpu_batch_text_value(line, "exact=")) {
        out.gpu_cigar_exact_jobs = *value;
    }
    if (auto value = gpu_batch_text_value(line, "unit_gap=")) {
        out.gpu_cigar_unit_gap_jobs = *value;
    }
    if (auto value = gpu_batch_text_value(line, "gridded=")) {
        out.gpu_cigar_gridded_jobs = *value;
    }
    if (auto value = gpu_batch_text_value(line, "query_symbols=")) {
        out.gpu_cigar_query_symbols = *value;
    }
    if (auto value = gpu_batch_text_value(line, "target_symbols=")) {
        out.gpu_cigar_target_symbols = *value;
    }
    if (auto value = gpu_batch_text_value(line, "packed_words=")) {
        out.gpu_cigar_packed_words = *value;
    }
    if (auto value = gpu_batch_text_value(line, "traceback_words=")) {
        out.gpu_cigar_traceback_words = *value;
    }
    if (auto value = gpu_batch_text_value(line, "prepare_ns=")) {
        out.gpu_cigar_prepare_ns = *value;
    }
    if (auto value = gpu_batch_text_value(line, "symbol_pack_ns=")) {
        out.gpu_cigar_symbol_pack_ns = *value;
    }
    if (auto value = gpu_batch_text_value(line, "classify_ns=")) {
        out.gpu_cigar_classify_ns = *value;
    }
    if (auto value = gpu_batch_text_value(line, "runtime_ns=")) {
        out.gpu_cigar_runtime_ns = *value;
    }
    if (auto value = gpu_batch_text_value(line, "decode_ns=")) {
        out.gpu_cigar_decode_ns = *value;
    }
}

void capture_gpu_explain_text_metrics(
        const it::connection_test_context& c,
        const std::string& align_sql,
        E2eOutput& out)
{
    const it::rowset_t explain_rows = it::query_rows(
        c,
        "EXPLAIN (ANALYZE, VERBOSE, FORMAT TEXT) " + align_sql);
    for (const auto& row : explain_rows) {
        if (row.empty() || !row.front().has_value()) {
            continue;
        }
        const std::string& line = *row.front();
        if (line.find("GPU batch [") != std::string::npos
            || line.find("GPU CIGAR [") != std::string::npos) {
            assign_gpu_text_metrics(line, out);
        }
    }
}

void capture_gpu_explain_metrics(
        const it::connection_test_context& c,
        const std::string& align_sql,
        E2eOutput& out)
{
    const it::rowset_t explain_rows = it::query_rows(
        c,
        "EXPLAIN (ANALYZE, VERBOSE, FORMAT JSON) " + align_sql);
    if (explain_rows.empty() || explain_rows.front().empty()
        || !explain_rows.front()[0].has_value()) {
        return;
    }
    simdjson::dom::parser parser;
    simdjson::dom::element doc;
    const std::string& json_text = *explain_rows.front()[0];
    if (parser.parse(json_text).get(doc) != simdjson::SUCCESS) {
        capture_gpu_explain_text_metrics(c, align_sql, out);
        return;
    }
    walk_gpu_explain_json(doc, out);
    if (out.gpu_problem_count < 0 || out.gpu_batched_branch_taken < 0) {
        capture_gpu_explain_text_metrics(c, align_sql, out);
    }
}

// minimap2 PAF: tab-separated; col indices (0-based): 4 strand(+/-), 5 target name
// (refN), 7 target start (per-record, forward strand), 8 target end. Primary = the
// line with tag tp:A:P; absolute target start = record_index*minimap2_split_bp +
// per_record_start. match_count counts PRIMARY alignments only (MEDIUM-04).
void parse_minimap2_paf(const std::string& paf, const E2eInput& in, E2eOutput& out) {
    std::int64_t best_abs = -1; bool best_rev = false; std::int64_t best_span = -1;
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
        const std::string_view rname = col[5];                 // "refN"
        const std::int64_t rec_idx = (rname.size() > 3) ? parse_i64(rname.substr(3)) : 0;
        const std::int64_t rstart  = parse_i64(col[7]);
        const std::int64_t abs_start =
            rec_idx * static_cast<std::int64_t>(in.minimap2_split_bp) + rstart;
        const std::int64_t span = parse_i64(col[8]) - rstart;  // target end - start
        const bool is_primary = (line.find("tp:A:P") != std::string_view::npos);
        if (is_primary) ++primaries;
        if (is_primary || span > best_span) {
            best_span = span; best_abs = abs_start; best_rev = (col[4] == "-");
            if (is_primary) best_span = std::numeric_limits<std::int64_t>::max();  // pin primary
        }
    }
    out.match_count   = primaries;                             // primary hits (MEDIUM-04)
    out.found         = (best_abs >= 0);
    out.target_start  = best_abs;
    out.is_reverse    = best_rev;
    out.abs_offset_bp = out.found
        ? static_cast<double>(std::llabs(best_abs - in.truth_target_start))
        : kNoMatchOffsetSentinel;
}

// HIGH-09/TESTING-04: the ground truth is known, so any miss or mis-map is ALWAYS
// a bug. Fail loudly rather than letting a fast, meaningless timing be reported
// as a "win". Called AFTER the timed region.
inline void require_mapped(std::string_view algo, const E2eInput& in,
                           const E2eOutput& out) {
    if (out.found && out.abs_offset_bp <= static_cast<double>(kPositionToleranceBp))
        return;
    bioseq_log<ERR>("[bench_e2e] {} failed to map the query at {} bp (found={}, "
         "target_start={}, truth={}, offset={} bp > {} tol) — the timed result is "
         "invalid; aborting", algo, in.target_bp, out.found, out.target_start,
         in.truth_target_start, out.abs_offset_bp, kPositionToleranceBp);
    // fmtlog is async; the ERR above can be lost when std::abort() runs before
    // the background poll flushes it. Emit a synchronous copy so the failing
    // cell is always visible in the captured stderr.
    std::fprintf(stderr,
        "[bench_e2e] MAP-FAIL algo=%.*s target_bp=%llu found=%d target_start=%lld "
        "truth=%lld offset_bp=%.1f tol=%lld — aborting\n",
        static_cast<int>(algo.size()), algo.data(),
        static_cast<unsigned long long>(in.target_bp), static_cast<int>(out.found),
        static_cast<long long>(out.target_start),
        static_cast<long long>(in.truth_target_start),
        out.abs_offset_bp, static_cast<long long>(kPositionToleranceBp));
    std::fflush(stderr);
    std::abort();
}

inline void require_gpu_batch_proof(std::string_view algo, const E2eInput& in,
                                    const E2eOutput& out) {
    if (algo != "bioseq_block_gpu_dp_gpu_seed_no_index"
        || in.queries.size() < 2) {
        return;
    }

    const auto expected = static_cast<std::int64_t>(in.queries.size());
    const auto fail = [algo, expected, &out](std::string_view field,
                                             std::int64_t got,
                                             std::int64_t want) {
        bioseq_log<ERR>(
            "[bench_e2e] GPU batch proof failed for {}: {}={} expected={} "
            "(queries={}, branch={}, problems={}, waves={}, seed_dispatches={}, "
            "chain_dispatches={}, extend_dispatches={}, idx_built={}, idx_reused={}, "
            "cpu_fallback_rows={}, simd_fallback_rows={}, declines={}, correct={}, "
            "correct_cigar={})",
            algo, field, got, want, expected, out.gpu_batched_branch_taken,
            out.gpu_problem_count, out.gpu_wave_count, out.gpu_seed_dispatches,
            out.gpu_chain_dispatches, out.gpu_extend_dispatches,
            out.gpu_segment_indexes_built, out.gpu_segment_indexes_reused,
            out.cpu_fallback_rows, out.simd_fallback_rows, out.vulkan_declines,
            out.correct_match_count, out.correct_cigar_count);
        std::fprintf(
            stderr,
            "[bench_e2e] GPU-BATCH-PROOF-FAIL algo=%.*s field=%.*s got=%lld "
            "expected=%lld queries=%lld branch=%lld problems=%lld waves=%lld "
            "seed_dispatches=%lld chain_dispatches=%lld extend_dispatches=%lld "
            "idx_built=%lld idx_reused=%lld cpu_fallback_rows=%lld "
            "simd_fallback_rows=%lld declines=%lld correct=%lld "
            "correct_cigar=%lld\n",
            static_cast<int>(algo.size()), algo.data(),
            static_cast<int>(field.size()), field.data(),
            static_cast<long long>(got), static_cast<long long>(want),
            static_cast<long long>(expected),
            static_cast<long long>(out.gpu_batched_branch_taken),
            static_cast<long long>(out.gpu_problem_count),
            static_cast<long long>(out.gpu_wave_count),
            static_cast<long long>(out.gpu_seed_dispatches),
            static_cast<long long>(out.gpu_chain_dispatches),
            static_cast<long long>(out.gpu_extend_dispatches),
            static_cast<long long>(out.gpu_segment_indexes_built),
            static_cast<long long>(out.gpu_segment_indexes_reused),
            static_cast<long long>(out.cpu_fallback_rows),
            static_cast<long long>(out.simd_fallback_rows),
            static_cast<long long>(out.vulkan_declines),
            static_cast<long long>(out.correct_match_count),
            static_cast<long long>(out.correct_cigar_count));
        std::fflush(stderr);
        std::abort();
    };

    const auto require_eq = [&fail](std::string_view field, std::int64_t got,
                                    std::int64_t want) {
        if (got != want) {
            fail(field, got, want);
        }
    };
    const auto require_ge = [&fail](std::string_view field, std::int64_t got,
                                    std::int64_t want) {
        if (got < want) {
            fail(field, got, want);
        }
    };

    constexpr std::int64_t kGpuProblemWave = 4096;
    const std::int64_t expected_waves =
        (expected + kGpuProblemWave - 1) / kGpuProblemWave;
    require_eq("gpu_batched_branch_taken", out.gpu_batched_branch_taken, 1);
    const bool expect_single_resident_target =
        in.batch_pair_mode && in.target_bp <= 1'000'000;
    if (expect_single_resident_target) {
        require_eq("gpu_problem_count", out.gpu_problem_count, expected);
        require_eq("gpu_wave_count", out.gpu_wave_count, expected_waves);
        require_eq("gpu_segment_indexes_built", out.gpu_segment_indexes_built, 1);
        require_eq("gpu_segment_indexes_reused", out.gpu_segment_indexes_reused,
                   expected > 0 ? expected - 1 : 0);
        require_eq("gpu_seed_dispatches", out.gpu_seed_dispatches,
                   2 * expected_waves);
        require_eq("gpu_chain_dispatches", out.gpu_chain_dispatches,
                   expected_waves);
    } else {
        require_ge("gpu_problem_count", out.gpu_problem_count, expected);
        require_ge("gpu_wave_count", out.gpu_wave_count, 1);
        require_ge("gpu_segment_indexes_built", out.gpu_segment_indexes_built, 1);
        require_ge("gpu_segment_indexes_reused", out.gpu_segment_indexes_reused,
                   expected > 0 ? expected - 1 : 0);
        require_ge("gpu_seed_dispatches", out.gpu_seed_dispatches, 2);
        require_ge("gpu_chain_dispatches", out.gpu_chain_dispatches, 1);
    }
    require_eq(
        "gpu_extend_dispatches",
        out.gpu_extend_dispatches,
        in.cigar_projection ? 1 : 0);
    require_eq("cpu_fallback_rows", out.cpu_fallback_rows, 0);
    require_eq("simd_fallback_rows", out.simd_fallback_rows, 0);
    require_eq("vulkan_declines", out.vulkan_declines, 0);
    require_eq("correct_match_count", out.correct_match_count, expected);
    if (in.cigar_projection) {
        require_eq("correct_cigar_count", out.correct_cigar_count, expected);
    }
}

// create_e2e_table: a USING bioseq table matching the active alphabet and the
// E2E generator's uint64 storage width.
void create_e2e_table(it::connection_test_context& c, const std::string& t,
                      const std::string& seq_type) {
    it::exec_sqlf(c, "DROP TABLE IF EXISTS {} CASCADE;", t);
    it::exec_sqlf(c, "CREATE TABLE {} (header text, seq {}) USING bioseq;",
                  t, seq_type);
}

void create_e2e_batch_pair_table(
        it::connection_test_context& c,
        const std::string& table,
        const std::string& seq_type) {
    it::exec_sqlf(c, "DROP TABLE IF EXISTS {} CASCADE;", table);
    it::exec_sqlf(
        c,
        "CREATE TABLE {} (id int PRIMARY KEY, header text, seq {}, query_seq {}) "
        "USING bioseq;",
        table, seq_type, seq_type);
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
        std::string_view minimizer_index) {
    if (minimizer_index == kMinimizersPerSequenceIndex) {
        return in.just_index_table_per_sequence;
    }
    return in.just_index_table_per_fragment;
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
        "ORDER BY index_name;",
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
                      in.just_index_table_per_fragment);
        it::exec_sqlf(*in.conn, "DROP TABLE IF EXISTS {} CASCADE;",
                      in.just_index_table_per_sequence);
        it::exec_sqlf(*in.conn, "DROP TABLE IF EXISTS {} CASCADE;", in.no_index_table);
        it::exec_sqlf(*in.conn, "DROP TABLE IF EXISTS {} CASCADE;", in.batch_pair_table);
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

void load_e2e_batch_pair_table(
        const it::connection_test_context& c,
        const E2eInput& in);

void verify_e2e_batch_pair_table(
        const it::connection_test_context& c,
        const E2eInput& in);

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
    gp.use_vulkan             = true;  // hard-fail if GPU unavailable: catches broken Vulkan early

    E2eInput in;
    in.alphabet    = std::string{bench_alphabet_name<A>()};
    in.seq_type    = bench_seq_type<A>();
    in.size_label  = jstr(data, "size_label", "x");
    in.kmer_size   = static_cast<int>(jint(data, "kmer_size", kDefaultKmerSize));
    in.window_size = static_cast<int>(jint(data, "window_size", kDefaultWindowSize));
    in.batch_pair_mode = jbool(data, "batch_pair_mode", false);
    in.batch_pair_load_timed = jbool(data, "batch_pair_load_timed", false);
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
    in.just_index_table_per_fragment =
        fmt::format("e2e_{}_just_mpf", in.size_label);
    in.just_index_table_per_sequence =
        fmt::format("e2e_{}_just_mps", in.size_label);
    in.just_index_table = in.just_index_table_per_sequence;
    in.no_index_table = fmt::format("e2e_{}_noidx", in.size_label);
    in.batch_pair_table = fmt::format("e2e_{}_batch_pair", in.size_label);

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
    if (gp.reference_length <= 1'000'000u) {
        in.target_bases = first.target->to_string();
    }
    write_multi_query_fasta<A>(in.query_fasta, in.queries);

    // (3) connect once; clean slate.
    in.conn = std::make_shared<it::connection_test_context>();
    it::reset_extension(*in.conn, { std::string_view{in.just_index_table_per_fragment},
                                    std::string_view{in.just_index_table_per_sequence},
                                    std::string_view{in.no_index_table},
                                    std::string_view{in.batch_pair_table} });
    preflight_e2e_resources(*in.conn, in.size_label, in.target_bp);

    // (4) force the fastest bioseq combo for the whole session.
    configure_bioseq_aligner_session(*in.conn, in.kmer_size, in.window_size);

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
                std::string_view index_name) {
            create_e2e_table(*in.conn, table, in.seq_type);
            configure_bioseq_scheduler_session(
                *in.conn, just_index_setup_pipeline_mode);
            // DOC0345 Patch L — Vulkan minimizer + GPU-index GUCs BEFORE the
            // persisted build so the untimed just_index build runs the GPU
            // minimizer extraction. (The spec's
            // configure_bioseq_pair_sort_session(*in.conn, params) is
            // intentionally omitted: ParamMap `params` is not in scope inside
            // e2e_input_typed, and the per-window pair sort on the Vulkan path is
            // host-side, not the CPU pair-sort GUC — see DOC0348.)
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
            if (audit_rows.empty()) {
                bioseq_log<ERR>(
                    "[bench_e2e] missing GPU just_index setup audit for table={} index={}",
                    table,
                    index_name);
                std::abort();
            }
        };

    // (5) just_index tables: each indexed cell gets its own table and policy.
    // This avoids a per-sequence run accidentally succeeding through a residual
    // per-fragment index, and keeps the per-fragment cell valid too.
    if (!skip_index_baseline_setup) {
        if (should_setup_just_index(kMinimizersPerFragmentIndex)) {
            setup_just_index_table(
                in.just_index_table_per_fragment, kMinimizersPerFragmentIndex);
        }
        if (should_setup_just_index(kMinimizersPerSequenceIndex)) {
            setup_just_index_table(
                in.just_index_table_per_sequence, kMinimizersPerSequenceIndex);
        }
    } else {
        bioseq_log<DBG>("[bench_e2e] skipping persisted-index/minimap2 baseline setup "
                "because BIOSEQ_E2E_SKIP_INDEX_BASELINE_SETUP=1");
    }

    // (6) no_index table: create empty; reps load into it inside the region.
    create_e2e_table(*in.conn, in.no_index_table, in.seq_type);
    if (in.batch_pair_mode) {
        create_e2e_batch_pair_table(
            *in.conn, in.batch_pair_table, in.seq_type);
        configure_bioseq_minimizer_policy(
            *in.conn, in.batch_pair_table, kMinimizersPerFragmentIndex);
        it::exec_sql(*in.conn, "SET bioseq.enable_bioseq_index_scan = off;");
        it::exec_sql(*in.conn, "SET bioseq.update_index_on_insert = off;");
        it::exec_sql(*in.conn, "SET bioseq.read_sequence_index   = off;");
        if (!in.batch_pair_load_timed) {
            load_e2e_batch_pair_table(*in.conn, in);
            verify_e2e_batch_pair_table(*in.conn, in);
        }
    }

    // (7) minimap2 prebuilt .mmi for the just_index baseline (untimed).
    if (!skip_index_baseline_setup && !skip_minimap2_baseline_setup) {
        run_cmd(fmt::format("{} -k{} -w{} -d {} {}", BIOSEQ_E2E_MINIMAP2_BIN,
                            in.kmer_size, in.window_size, in.mmi_path, in.target_fasta_split));
    } else if (skip_minimap2_baseline_setup) {
        bioseq_log<DBG>("[bench_e2e] skipping minimap2 .mmi baseline setup because "
                "BIOSEQ_E2E_SKIP_MINIMAP2_BASELINE_SETUP=1");
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
// When the env var is unset the restart is skipped with a loud warning so the
// harness stays portable and unrelated configs/runs are unaffected.
// Returns true iff a restart actually happened.
bool restart_postgres_for_cold_just_index(const E2eInput& in) {
    const char* cmd = std::getenv("BIOSEQ_E2E_PG_RESTART_CMD");
    if (cmd == nullptr || *cmd == '\0') {
        bioseq_log<WRN>("[bench_e2e] BIOSEQ_E2E_PG_RESTART_CMD is unset — skipping "
            "the pre-just_index PostgreSQL restart; the index may still be "
            "resident so this just_index timing is NOT guaranteed to be a cold read");
        return false;
    }
    bioseq_log<INF>("[bench_e2e] restarting PostgreSQL before just_index measure "
        "(evict resident/cached index for a cold assemble-from-storage): {}", cmd);
    const int rc = std::system(cmd);
    if (rc != 0) {
        bioseq_log<ERR>("[bench_e2e] PostgreSQL restart command failed (rc={}): {} "
            "— the cold just_index measurement would be invalid; aborting", rc, cmd);
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
    // The fresh session lost the on_input aligner GUCs (block aligner + SIMD-mm2
    // chaining + two-stack nthash minimizer) that the just_index paths assume.
    configure_bioseq_aligner_session(*in.conn, in.kmer_size, in.window_size);
    bioseq_log<INF>("[bench_e2e] PostgreSQL restarted + session reconnected; the "
        "just_index read will assemble the index cold from storage");
    return true;
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

void load_e2e_batch_pair_table(
        const it::connection_test_context& c,
        const E2eInput& in)
{
    it::exec_sql(c, "SET bioseq.enable_bioseq_index_scan = off;");
    it::exec_sqlf(c, "TRUNCATE {};", in.batch_pair_table);
    constexpr std::int64_t kLiteralTargetMaxBp = 1'000'000;
    const bool stage_target_once =
        in.target_bases.empty() || in.target_bp > kLiteralTargetMaxBp;
    const std::string shared_target_table =
        fmt::format("{}_shared_target", in.batch_pair_table);
    if (stage_target_once) {
        it::exec_sqlf(c, "DROP TABLE IF EXISTS {} CASCADE;", shared_target_table);
        it::exec_sqlf(
            c,
            "CREATE UNLOGGED TABLE {} "
            "(header text, seq {}) USING bioseq;",
            shared_target_table,
            in.seq_type);
        it::exec_sqlf(
            c,
            "SELECT bioseq.load('{}'::regclass, '{}');",
            shared_target_table,
            in.target_fasta);
    }
    constexpr std::size_t kInsertChunkRows = 32;
    for (std::size_t first = 0; first < in.queries.size(); first += kInsertChunkRows) {
        const std::size_t last =
            std::min(in.queries.size(), first + kInsertChunkRows);
        if (stage_target_once) {
            std::string sql = fmt::format(
                "INSERT INTO {} (id, header, seq, query_seq) "
                "SELECT v.id, v.header, t.seq, v.query_seq "
                "FROM (VALUES ",
                in.batch_pair_table);
            for (std::size_t i = first; i < last; ++i) {
                const E2eQueryTruth& query = in.queries[i];
                if (i != first) {
                    sql += ",";
                }
                sql += fmt::format(
                    "({}, 'ref_{}', '{}'::{})",
                    query.id,
                    query.id,
                    query.bases,
                    in.seq_type);
            }
            sql += ") AS v(id, header, query_seq) "
                   + fmt::format(
                       "CROSS JOIN (SELECT seq FROM {} ORDER BY header LIMIT 1) AS t;",
                       shared_target_table);
            it::exec_sql(c, sql);
        } else {
            std::string sql = fmt::format(
                "INSERT INTO {} (id, header, seq, query_seq) VALUES ",
                in.batch_pair_table);
            for (std::size_t i = first; i < last; ++i) {
                const E2eQueryTruth& query = in.queries[i];
                if (i != first) {
                    sql += ",";
                }
                sql += fmt::format(
                    "({}, 'ref_{}', '{}'::{}, '{}'::{})",
                    query.id,
                    query.id,
                    in.target_bases,
                    in.seq_type,
                    query.bases,
                    in.seq_type);
            }
            it::exec_sql(c, sql);
        }
    }
    if (stage_target_once) {
        it::exec_sqlf(c, "DROP TABLE IF EXISTS {} CASCADE;", shared_target_table);
    }
}

void verify_e2e_batch_pair_table(
        const it::connection_test_context& c,
        const E2eInput& in)
{
    it::exec_sql(c, "SET bioseq.enable_bioseq_index_scan = off;");
    const auto rows = it::query_rows(c, fmt::format(
        "SELECT id FROM {} ORDER BY id", in.batch_pair_table));
    bool ids_match = rows.size() == in.queries.size();
    if (ids_match) {
        for (std::size_t i = 0; i < rows.size(); ++i) {
            if (rows[i].empty() || !rows[i][0].has_value()
                || parse_i64(*rows[i][0]) != static_cast<std::int64_t>(i)) {
                ids_match = false;
                break;
            }
        }
    }
    if (!ids_match) {
        const std::string msg = fmt::format(
            "[bench_e2e] FATAL batch-pair load visibility mismatch: "
            "table={} visible_rows={} expected_rows={}\n",
            in.batch_pair_table,
            rows.size(),
            in.queries.size());
        std::fputs(msg.c_str(), stderr);
        std::fflush(stderr);
        std::abort();
    }
}

[[nodiscard]] std::string batched_coord_sql(
        const std::string& target_table,
        std::string_view projection)
{
    return fmt::format(
        "SELECT q.id, t.header, {} "
        "FROM {} t JOIN e2e_queries q ON mmsearch(t.seq, q.seq) "
        "ORDER BY q.id, t.header",
        projection, target_table);
}

[[nodiscard]] std::string batch_pair_coord_sql(
        const std::string& pair_table,
        std::string_view projection)
{
    return fmt::format(
        "SELECT id, header, {} "
        "FROM {} WHERE mmsearch(seq, query_seq) "
        "ORDER BY id, header",
        projection, pair_table);
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
    const int minimap_threads = std::max(1, param_int(params, "threads", 1));
    const std::string minimizer_index = param_minimizer_index(params);
    const std::string qlit = fmt::format("'{}'::{}", in.query_bases, in.seq_type);
    const std::string proj = e2e_projection_sql(in);

    if (algo == "bioseq_just_index"
        || algo == "bioseq_block_simd_dp_two_stack_nthash_just_index") {
        // Restart PostgreSQL first so the prebuilt index (built untimed in
        // on_input) is assembled cold from storage rather than served warm.
        restart_postgres_for_cold_just_index(in);
        configure_bioseq_scheduler_session(*in.conn, pipeline_mode);
        configure_bioseq_pair_sort_session(*in.conn, params);
        const std::string& table = just_index_table_for(in, minimizer_index);
        configure_bioseq_minimizer_policy(
            *in.conn, table, minimizer_index);
        it::exec_sql(*in.conn, "SET bioseq.read_sequence_index = on;");
        const std::string sql = fmt::format(
            "SELECT header, {} FROM {} WHERE mmsearch(seq, {}) ORDER BY header",
            proj, table, qlit);
        ScopedResourceMonitor resources;
        const auto start_ns = static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
        {
            auto _scope = bioseq::instr::region(kMeasuredRegion); // TIMED: assemble only
            parse_bioseq_rows(it::query_rows(*in.conn, sql), in, out);
        }
        const auto end_ns = static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
        attach_resource_metrics(out, start_ns, end_ns, resources.stop());
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
        restart_postgres_for_cold_just_index(in);
        configure_bioseq_scheduler_session(*in.conn, pipeline_mode);
        configure_bioseq_pair_sort_session(*in.conn, params);
        const std::string& table = just_index_table_for(in, minimizer_index);
        configure_bioseq_minimizer_policy(*in.conn, table, minimizer_index);
        it::exec_sql(*in.conn, "SET bioseq.read_sequence_index = on;");
        configure_bioseq_vulkan_index_session(
            *in.conn,
            in.kmer_size,
            in.window_size,
            in.cigar_projection);  // DOC0345 Patch L
        const std::string sql = fmt::format(
            "SELECT header, {} FROM {} WHERE mmsearch(seq, {}) ORDER BY header",
            proj, table, qlit);
        ScopedResourceMonitor resources;
        const auto start_ns = static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
        {
            auto _scope = bioseq::instr::region(kMeasuredRegion); // TIMED: assemble only
            parse_bioseq_rows(it::query_rows(*in.conn, sql), in, out);
        }
        const auto end_ns = static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
        attach_resource_metrics(out, start_ns, end_ns, resources.stop());
        require_mapped(algo, in, out);
        capture_mps_build_stats(*in.conn, out);
        capture_index_audit(*in.conn, table, minimizer_index, out);
        log_measured_case(algo, pipeline_mode, minimap_threads, in, out, minimizer_index);

    } else if (algo == "bioseq_no_index"
               || algo == "bioseq_block_simd_dp_two_stack_nthash_no_index") {
        const bool recompute_async_persist =
            param_string(params, "recompute_async_persist", "off") == "on";
        configure_bioseq_scheduler_session(*in.conn, pipeline_mode);
        configure_bioseq_pair_sort_session(*in.conn, params);
        it::exec_sqlf(*in.conn, "TRUNCATE {};", in.no_index_table);          // reset (untimed)
        configure_bioseq_minimizer_policy(
            *in.conn,
            in.batch_pair_mode ? in.batch_pair_table : in.no_index_table,
            minimizer_index);
        it::exec_sql(*in.conn, "SET bioseq.update_index_on_insert = off;");  // load builds NO index
        it::exec_sql(*in.conn, "SET bioseq.read_sequence_index   = off;");   // align recomputes
        const std::string load_sql = fmt::format(
            "SELECT bioseq.load('{}'::regclass, '{}')", in.no_index_table, in.target_fasta);
        const std::string align_sql = in.batch_pair_mode
            ? batch_pair_coord_sql(in.batch_pair_table, proj)
            : batched_coord_sql(in.no_index_table, proj);
        ScopedResourceMonitor resources;
        const auto start_ns = static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
        std::optional<it::connection_test_context> read_conn;
        {
            auto _scope = bioseq::instr::region(kMeasuredRegion); // TIMED: load + recompute align
            if (!in.batch_pair_mode) {
                it::exec_sql(*in.conn, load_sql);
            }
            // A same-backend custom scan immediately after TRUNCATE + bioseq.load
            // can observe stale columnar descriptor state. Use a fresh backend for
            // the measured read; connection setup remains inside the timed region.
            read_conn.emplace();
            configure_bioseq_aligner_session(*read_conn, in.kmer_size, in.window_size);
            configure_bioseq_scheduler_session(*read_conn, pipeline_mode);
            configure_bioseq_pair_sort_session(*read_conn, params);
            configure_bioseq_minimizer_policy(
                *read_conn,
                in.batch_pair_mode ? in.batch_pair_table : in.no_index_table,
                minimizer_index);
            it::exec_sql(*read_conn, "SET bioseq.update_index_on_insert = off;");
            it::exec_sql(*read_conn, "SET bioseq.read_sequence_index   = off;");
            it::exec_sqlf(*read_conn, "SET bioseq.recompute_async_persist = {};",
                          recompute_async_persist ? "on" : "off");
            if (in.batch_pair_mode) {
                it::exec_sql(
                    *read_conn,
                    "SET bioseq.enable_bioseq_index_scan = off;");
                if (in.batch_pair_load_timed) {
                    load_e2e_batch_pair_table(*read_conn, in);
                    verify_e2e_batch_pair_table(*read_conn, in);
                }
            } else {
                load_e2e_query_table(*read_conn, in);
            }
            parse_bioseq_batch_rows(it::query_rows(*read_conn, align_sql), in, out);
            capture_mps_build_stats(*read_conn, out);
        }
        const auto end_ns = static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
        attach_resource_metrics(out, start_ns, end_ns, resources.stop());
        require_mapped(algo, in, out);
        if (recompute_async_persist) {
            capture_index_audit_after_async_persist(
                *in.conn,
                in.batch_pair_mode ? in.batch_pair_table : in.no_index_table,
                minimizer_index, out);
            capture_mps_build_stats(*in.conn, out);
        } else {
            capture_index_audit(
                *in.conn,
                in.batch_pair_mode ? in.batch_pair_table : in.no_index_table,
                minimizer_index, out);
        }
        log_measured_case(
            algo, pipeline_mode, minimap_threads, in, out, minimizer_index);

    } else if (algo == "bioseq_block_gpu_dp_gpu_seed_no_index") {
        // GPU (DOC0310 Phase C Vulkan) frontend on the no_index path: no prebuilt
        // index — the timed region loads the target and recomputes minimizers with
        // GPU seeding + tiled-DP chaining + block aligner driving the alignment.
        // Mirrors bioseq_no_index but swaps the chaining/minimizer to the Vulkan
        // frontend on the fresh read connection (same GUCs as the gpu_dp_gpu_seed
        // just_index path). No restart: no_index measures the full from-scratch
        // build+map cost, so there is no prebuilt index to evict.
        const bool recompute_async_persist =
            param_string(params, "recompute_async_persist", "off") == "on";
        configure_bioseq_scheduler_session(*in.conn, pipeline_mode);
        configure_bioseq_pair_sort_session(*in.conn, params);
        it::exec_sqlf(*in.conn, "TRUNCATE {};", in.no_index_table);          // reset (untimed)
        configure_bioseq_minimizer_policy(
            *in.conn,
            in.batch_pair_mode ? in.batch_pair_table : in.no_index_table,
            minimizer_index);
        it::exec_sql(*in.conn, "SET bioseq.update_index_on_insert = off;");  // load builds NO index
        it::exec_sql(*in.conn, "SET bioseq.read_sequence_index   = off;");   // align recomputes
        const std::string load_sql = fmt::format(
            "SELECT bioseq.load('{}'::regclass, '{}')", in.no_index_table, in.target_fasta);
        const std::string align_sql = in.batch_pair_mode
            ? batch_pair_coord_sql(in.batch_pair_table, proj)
            : batched_coord_sql(in.no_index_table, proj);
        ScopedResourceMonitor resources;
        const auto start_ns = static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
        std::optional<it::connection_test_context> read_conn;
        {
            auto _scope = bioseq::instr::region(kMeasuredRegion); // TIMED: load + GPU recompute align
            if (!in.batch_pair_mode) {
                it::exec_sql(*in.conn, load_sql);
            }
            // Fresh backend for the measured read (see bioseq_no_index rationale);
            // connection setup stays inside the timed region.
            read_conn.emplace();
            configure_bioseq_aligner_session(*read_conn, in.kmer_size, in.window_size);
            configure_bioseq_scheduler_session(*read_conn, pipeline_mode);
            configure_bioseq_pair_sort_session(*read_conn, params);
            configure_bioseq_minimizer_policy(
                *read_conn,
                in.batch_pair_mode ? in.batch_pair_table : in.no_index_table,
                minimizer_index);
            it::exec_sql(*read_conn, "SET bioseq.update_index_on_insert = off;");
            it::exec_sql(*read_conn, "SET bioseq.read_sequence_index   = off;");
            it::exec_sqlf(*read_conn, "SET bioseq.recompute_async_persist = {};",
                          recompute_async_persist ? "on" : "off");
            // GPU/Vulkan phase-C frontend (same GUCs as bioseq_block_gpu_dp_gpu_seed_just_index).
            configure_bioseq_vulkan_index_session(
                *read_conn,
                in.kmer_size,
                in.window_size,
                in.cigar_projection);  // DOC0345 Patch L
            if (in.batch_pair_mode) {
                it::exec_sql(
                    *read_conn,
                    "SET bioseq.enable_bioseq_index_scan = off;");
                if (in.batch_pair_load_timed) {
                    load_e2e_batch_pair_table(*read_conn, in);
                    verify_e2e_batch_pair_table(*read_conn, in);
                }
            } else {
                load_e2e_query_table(*read_conn, in);
            }
            parse_bioseq_batch_rows(it::query_rows(*read_conn, align_sql), in, out);
            capture_mps_build_stats(*read_conn, out);
        }
        const auto end_ns = static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
        if (read_conn.has_value()) {
            capture_gpu_explain_metrics(*read_conn, align_sql, out);
        }
        require_gpu_batch_proof(algo, in, out);
        attach_resource_metrics(out, start_ns, end_ns, resources.stop());
        require_mapped(algo, in, out);
        if (recompute_async_persist) {
            capture_index_audit_after_async_persist(
                *in.conn,
                in.batch_pair_mode ? in.batch_pair_table : in.no_index_table,
                minimizer_index, out);
            capture_mps_build_stats(*in.conn, out);
        } else {
            capture_index_audit(
                *in.conn,
                in.batch_pair_mode ? in.batch_pair_table : in.no_index_table,
                minimizer_index, out);
        }
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
            "SELECT header, {} FROM {} WHERE mmsearch(seq, {}) ORDER BY header",
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
        {"gpu_batched_branch_taken", static_cast<double>(o.gpu_batched_branch_taken)},
        {"gpu_problem_count", static_cast<double>(o.gpu_problem_count)},
        {"gpu_wave_count", static_cast<double>(o.gpu_wave_count)},
        {"gpu_segment_indexes_built", static_cast<double>(o.gpu_segment_indexes_built)},
        {"gpu_segment_indexes_reused", static_cast<double>(o.gpu_segment_indexes_reused)},
        {"gpu_seed_dispatches", static_cast<double>(o.gpu_seed_dispatches)},
        {"gpu_chain_dispatches", static_cast<double>(o.gpu_chain_dispatches)},
        {"gpu_extend_dispatches", static_cast<double>(o.gpu_extend_dispatches)},
        {"cpu_fallback_rows", static_cast<double>(o.cpu_fallback_rows)},
        {"simd_fallback_rows", static_cast<double>(o.simd_fallback_rows)},
        {"vulkan_declines", static_cast<double>(o.vulkan_declines)},
        {"gpu_cigar_jobs", static_cast<double>(o.gpu_cigar_jobs)},
        {"gpu_cigar_bounded_jobs", static_cast<double>(o.gpu_cigar_bounded_jobs)},
        {"gpu_cigar_exact_jobs", static_cast<double>(o.gpu_cigar_exact_jobs)},
        {"gpu_cigar_unit_gap_jobs", static_cast<double>(o.gpu_cigar_unit_gap_jobs)},
        {"gpu_cigar_gridded_jobs", static_cast<double>(o.gpu_cigar_gridded_jobs)},
        {"gpu_cigar_query_symbols", static_cast<double>(o.gpu_cigar_query_symbols)},
        {"gpu_cigar_target_symbols", static_cast<double>(o.gpu_cigar_target_symbols)},
        {"gpu_cigar_packed_words", static_cast<double>(o.gpu_cigar_packed_words)},
        {"gpu_cigar_traceback_words", static_cast<double>(o.gpu_cigar_traceback_words)},
        {"gpu_cigar_prepare_ns", static_cast<double>(o.gpu_cigar_prepare_ns)},
        {"gpu_cigar_symbol_pack_ns", static_cast<double>(o.gpu_cigar_symbol_pack_ns)},
        {"gpu_cigar_classify_ns", static_cast<double>(o.gpu_cigar_classify_ns)},
        {"gpu_cigar_runtime_ns", static_cast<double>(o.gpu_cigar_runtime_ns)},
        {"gpu_cigar_decode_ns", static_cast<double>(o.gpu_cigar_decode_ns)},
        {"measured_region_ns_manual", static_cast<double>(o.measured_region_ns)},
        {"peak_postgres_rss_mb",    static_cast<double>(o.peak_postgres_rss_kb) / 1024.0},
        {"peak_minimap2_rss_mb",    static_cast<double>(o.peak_minimap2_rss_kb) / 1024.0},
        {"peak_bench_rss_mb",       static_cast<double>(o.peak_bench_rss_kb) / 1024.0},
        {"min_tmp_available_gib",   static_cast<double>(o.min_tmp_available_kb) / (1024.0 * 1024.0)},
        {"peak_swap_used_mb",       o.peak_swap_used_mb},
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

}  // anonymous namespace
HWY_AFTER_NAMESPACE();    // close the Highway per-target namespace

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
    return bioseq::bench_framework::run(argc, argv, rep, data,
        bioseq::bench_framework::create_benchmark<E2eInput, E2eOutput>(
            "End-to-end SQL bioseq.load + mmsearch vs minimap2 (2/4 Gbp dna4/dna5)",
            "bench_e2e", cfg, est, inp, run, evl, cln));
}
