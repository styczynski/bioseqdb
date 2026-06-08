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
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

import mlogging;
import bench_framework;
import bioseq_common.templating;           // CompTimeStr for alphabet_entry
import bioseq_instr;                       // region(), register_region_static
import integration_tests.common;           // connection_test_context, exec_sqlf, query_rows
import sequan_compat;                      // seq::dna4 / seq::dna5
import bioseq_sequences;                   // compact_sequence_range, traits, supported_alphabets, to_string()
import bioseq_sequences.generation;        // GeneratorParams / generate_test_sequences<A,U>
import bioseq_io.writer;                    // sequence_writer<A, FASTA>
import bioseq_io.public_api;                // compact_sequence_view<R>
import bioseq_io.options;                   // sequence_io_file_type, writer_options

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
// The generator types live in bioseq::sequences::generation::HWY_NAMESPACE (the
// generator uses Highway SIMD). The generator is now templated over
// <alphabet, storage_unit> (§10.0a).
using bioseq::sequences::generation::HWY_NAMESPACE::GeneratorParams;
using bioseq::sequences::generation::HWY_NAMESPACE::generate_test_sequences;
using bioseq::sequences::generation::HWY_NAMESPACE::GeneratedBenchSequencePair;
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
using bench_pair_t = GeneratedBenchSequencePair<A, bench_storage_t>;

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

struct E2eInput {
    // Connection is shared (const-usable; SQL mutates the DB, not this struct).
    std::shared_ptr<it::connection_test_context> conn;
    std::string alphabet;                        // "dna4" / "dna5"
    std::string seq_type;                        // bioseq.seq('<alphabet>', 64)
    std::string size_label;                      // "2gbp" / "4gbp"
    std::string just_index_table;                // loaded + indexed in on_input
    std::string no_index_table;                  // empty; loaded inside the region
    std::string target_fasta;                    // single-record, for bioseq.load
    std::string target_fasta_split;              // <=split_bp records, for minimap2
    std::string query_fasta;                     // for minimap2
    std::string mmi_path;                        // prebuilt minimap2 index
    std::string query_bases;                     // ACGTN text for the bioseq SQL literal
    int kmer_size = 15, window_size = 10;
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
    double abs_offset_bp = 0.0;
    std::uint64_t measured_region_ns = 0;
    std::uint64_t peak_postgres_rss_kb = 0;
    std::uint64_t peak_minimap2_rss_kb = 0;
    std::uint64_t peak_bench_rss_kb = 0;
    std::uint64_t min_tmp_available_kb = 0;
    double peak_swap_used_mb = 0.0;
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
        loge("[bench_e2e] {} requires >=50 GiB free in /tmp and the PG data "
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
        [&]<class Entry>() {
            using alphabet_t = typename Entry::alphabet_type;
            handler.template operator()<alphabet_t>();
        });
    if (!matched) {
        loge("[bench_e2e] unsupported alphabet '{}'", name);
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
        loge("[bench_e2e] cannot open '{}': {}", path, writer_or.error()); std::abort();
    }
    auto& writer = *writer_or;
    const auto emit = [&](std::size_t idx, std::size_t off,
                          std::size_t cnt, std::size_t pad) {
        if (!writer.write_record_streamed(fmt::format("{}{}", base_name, idx),
                                          view_slice<A>(seq, off, cnt, pad),
                                          kFastaStreamChunkBp)) {
            loge("[bench_e2e] write_record_streamed failed for '{}'", path); std::abort();
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
        loge("[bench_e2e] close '{}': {}", path, c.error()); std::abort();
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

/// Run a shell command, abort the bench on non-zero exit. Used for the untimed
/// .mmi build. (Inside the timed region we use run_cmd_capture.)
inline void run_cmd(const std::string& cmd) {
    const int rc = std::system(cmd.c_str());
    if (rc != 0) {
        loge("[bench_e2e] command failed (rc={}): {}", rc, cmd);
        std::abort();
    }
}

/// Run a shell command, capture stdout. Used for minimap2 -> PAF. Returns "" and
/// logs on spawn failure (the algorithm then reports found=false; see R3).
[[nodiscard]] inline std::string run_cmd_capture(const std::string& cmd) {
    std::string out;
    std::FILE* pipe = ::popen(cmd.c_str(), "r");
    if (pipe == nullptr) { loge("[bench_e2e] popen failed: {}", cmd); return out; }
    std::array<char, kPopenBufBytes> buf;
    std::size_t n;
    while ((n = std::fread(buf.data(), 1, buf.size(), pipe)) > 0) out.append(buf.data(), n);
    const int rc = ::pclose(pipe);
    if (rc != 0) {
        loge("[bench_e2e] command failed (rc={}): {}", rc, cmd);
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

// bioseq: the custom-scan projects one row per matched target row. Columns are
// [header, tstart, rev]; ORDER BY header. At fragment_count=1 there is exactly
// one target row.
void parse_bioseq_rows(const it::rowset_t& rows, const E2eInput& in, E2eOutput& out) {
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
    loge("[bench_e2e] {} failed to map the query at {} bp (found={}, "
         "target_start={}, truth={}, offset={} bp > {} tol) — the timed result is "
         "invalid; aborting", algo, in.target_bp, out.found, out.target_start,
         in.truth_target_start, out.abs_offset_bp, kPositionToleranceBp);
    std::abort();
}

// create_e2e_table: a USING bioseq table matching the active alphabet and the
// E2E generator's uint64 storage width.
void create_e2e_table(it::connection_test_context& c, const std::string& t,
                      const std::string& seq_type) {
    it::exec_sqlf(c, "DROP TABLE IF EXISTS {} CASCADE;", t);
    it::exec_sqlf(c, "CREATE TABLE {} (header text, seq {}) USING bioseq;",
                  t, seq_type);
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

void configure_bioseq_scheduler_session(it::connection_test_context& c,
                                        std::string_view pipeline_mode) {
    it::exec_sqlf(c, "SET bioseq.scheduler.pipeline_concurrency_mode = '{}';",
                  pipeline_mode);
    it::exec_sql(c, "SET bioseq.index_async_persist = on;");
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
                       const E2eOutput& out) {
    JustLog("[bench_e2e] measured algo={} pipeline_mode={} minimap_threads={} size={} measured_ns={} pg_peak_rss_mb={:.2f} minimap2_peak_rss_mb={:.2f} bench_peak_rss_mb={:.2f} tmp_min_free_gib={:.2f} swap_peak_mb={:.2f} found={} offset_bp={}",
            algo,
            pipeline_mode,
            minimap_threads,
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
        JustLog("[bench_e2e] keeping SQL tables and FASTA/MMI artifacts "
                "because BIOSEQ_E2E_KEEP_ARTIFACTS=1");
        return;
    }

    if (in.conn != nullptr) {
        it::exec_sqlf(*in.conn, "DROP TABLE IF EXISTS {} CASCADE;", in.just_index_table);
        it::exec_sqlf(*in.conn, "DROP TABLE IF EXISTS {} CASCADE;", in.no_index_table);
    }

    for (const auto& path : {in.target_fasta, in.target_fasta_split,
                             in.query_fasta, in.mmi_path}) {
        std::error_code ec;
        std::filesystem::remove(path, ec);
        if (ec) {
            logw("[bench_e2e] cleanup could not remove '{}': {}", path, ec.message());
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
        [&]<sequence_alphabet A>() { out = e2e_estimate_typed<A>(data); });
    return out;
}

// ---- §10.2 on_input (all untimed setup) ----
template <sequence_alphabet A>
[[nodiscard]] E2eInput e2e_input_typed(std::string_view /*data_dir*/,
                                       std::string_view test_case_name,
                                       simdjson::dom::element data,
                                       const RunConstraints&) {
    GeneratorParams gp{};
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

    E2eInput in;
    in.alphabet    = std::string{bench_alphabet_name<A>()};
    in.seq_type    = bench_seq_type<A>();
    in.size_label  = jstr(data, "size_label", "x");
    in.kmer_size   = static_cast<int>(jint(data, "kmer_size", kDefaultKmerSize));
    in.window_size = static_cast<int>(jint(data, "window_size", kDefaultWindowSize));
    in.minimap2_split_bp = static_cast<std::size_t>(jint(data, "minimap2_split_bp", kMinimap2SplitBp));
    const std::size_t split_bp = in.minimap2_split_bp;
    using active_traits = typename bench_sequence_range<A>::traits;
    if (split_bp > static_cast<std::size_t>(kMinimap2MaxRecordBp) ||
        split_bp % active_traits::letters_per_unit != 0) {
        loge("[bench_e2e] minimap2_split_bp={} is invalid for alphabet {} "
             "(letters_per_unit={}, max={})",
             split_bp, in.alphabet, active_traits::letters_per_unit,
             kMinimap2MaxRecordBp);
        std::abort();
    }

    const std::string tmp = "/tmp";
    in.target_fasta        = fmt::format("{}/e2e_{}_target.fasta",        tmp, in.size_label);
    in.target_fasta_split  = fmt::format("{}/e2e_{}_target.split.fasta",  tmp, in.size_label);
    in.query_fasta         = fmt::format("{}/e2e_{}_query.fasta",         tmp, in.size_label);
    in.mmi_path            = fmt::format("{}/e2e_{}_target.mmi",          tmp, in.size_label);
    in.just_index_table    = fmt::format("e2e_{}_just",                   in.size_label);
    in.no_index_table      = fmt::format("e2e_{}_noidx",                  in.size_label);

    // (1) generate ONE (query, target) pair (fragment_count is forced to 1).
    auto pairs = generate_test_sequences<A, bench_storage_t>(gp);
    const bench_pair_t<A>& pair = pairs.front();
    in.truth_target_start = static_cast<std::int64_t>(pair.truth_target_start);
    in.truth_is_reverse   = pair.is_reverse;
    in.target_bp          = static_cast<std::int64_t>(gp.reference_length);

    // (2) write FASTA via the bounded IO sequence_writer over zero-copy views.
    write_single_record_fasta<A>(in.target_fasta, "ref", pair.target);
    write_split_fasta<A>(in.target_fasta_split, "ref", pair.target, split_bp);
    in.query_bases = pair.query.to_string();   // generic decode, <=5 MB; may contain N for dna5
    write_single_record_fasta<A>(in.query_fasta, "query", pair.query);

    // (3) connect once; clean slate.
    in.conn = std::make_shared<it::connection_test_context>();
    it::reset_extension(*in.conn, { std::string_view{in.just_index_table},
                                    std::string_view{in.no_index_table} });
    preflight_e2e_resources(*in.conn, in.size_label, in.target_bp);

    // (4) force the fastest bioseq combo for the whole session.
    configure_bioseq_aligner_session(*in.conn, in.kmer_size, in.window_size);

    const char* skip_index_setup_env =
        std::getenv("BIOSEQ_E2E_SKIP_INDEX_BASELINE_SETUP");
    const bool skip_index_baseline_setup =
        skip_index_setup_env != nullptr
        && std::string_view{skip_index_setup_env} == "1";

    // (5) just_index table: create, load (build per-fragment), then build the
    //     per-sequence merged index and make it the active read index.
    if (!skip_index_baseline_setup) {
        create_e2e_table(*in.conn, in.just_index_table, in.seq_type);
        it::exec_sql(*in.conn, "SET bioseq.update_index_on_insert = on;");
        it::exec_sqlf(*in.conn, "SELECT bioseq.load('{}'::regclass, '{}');",
                      in.just_index_table, in.target_fasta);
        it::exec_sqlf(*in.conn, "SELECT bioseq.update_index('{}'::regclass, 'minimizers_per_sequence');",
                      in.just_index_table);
        it::exec_sqlf(*in.conn, "SELECT bioseq.set_active_read_index('{}'::regclass, 'minimizers_per_sequence');",
                      in.just_index_table);
    } else {
        JustLog("[bench_e2e] skipping persisted-index/minimap2 baseline setup "
                "because BIOSEQ_E2E_SKIP_INDEX_BASELINE_SETUP=1");
    }

    // (6) no_index table: create empty; reps load into it inside the region.
    create_e2e_table(*in.conn, in.no_index_table, in.seq_type);

    // (7) minimap2 prebuilt .mmi for the just_index baseline (untimed).
    if (!skip_index_baseline_setup) {
        run_cmd(fmt::format("{} -k{} -w{} -d {} {}", BIOSEQ_E2E_MINIMAP2_BIN,
                            in.kmer_size, in.window_size, in.mmi_path, in.target_fasta_split));
    }

    JustLog("[bench_e2e] input '{}' ready (truth_start={}, rev={})",
            test_case_name, in.truth_target_start, in.truth_is_reverse);
    return in;
}

[[nodiscard]] E2eInput e2e_input(std::string_view data_dir,
                                 std::string_view test_case_name,
                                 simdjson::dom::element data,
                                 const RunConstraints& constraints) {
    E2eInput out;
    dispatch_bench_alphabet(jstr(data, "alphabet", "dna4"),
        [&]<sequence_alphabet A>() {
            out = e2e_input_typed<A>(data_dir, test_case_name, data, constraints);
        });
    return out;
}

// ---- §10.3 on_run (the timed region, dispatched by mode) ----
[[nodiscard]] E2eOutput e2e_run(std::string_view algo, const ParamMap& params,
                                const E2eInput& in) {
    E2eOutput out;
    const std::string pipeline_mode = param_string(params, "pipeline_mode", "single");
    const int minimap_threads = std::max(1, param_int(params, "threads", 1));
    const std::string qlit = fmt::format("'{}'::{}", in.query_bases, in.seq_type);
    const std::string proj =
        "bioseq.target_start(header) AS tstart, bioseq.is_reverse(header) AS rev";

    if (algo == "bioseq_just_index") {
        configure_bioseq_scheduler_session(*in.conn, pipeline_mode);
        it::exec_sql(*in.conn, "SET bioseq.read_sequence_index = on;");
        const std::string sql = fmt::format(
            "SELECT header, {} FROM {} WHERE mmsearch(seq, {}) ORDER BY header",
            proj, in.just_index_table, qlit);
        ScopedResourceMonitor resources;
        const auto start_ns = static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
        {
            auto _scope = bioseq::instr::region(kMeasuredRegion); // TIMED: assemble only
            parse_bioseq_rows(it::query_rows(*in.conn, sql), in, out);
        }
        const auto end_ns = static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
        attach_resource_metrics(out, start_ns, end_ns, resources.stop());
        require_mapped(algo, in, out);
        log_measured_case(algo, pipeline_mode, minimap_threads, in, out);

    } else if (algo == "bioseq_no_index") {
        configure_bioseq_scheduler_session(*in.conn, pipeline_mode);
        it::exec_sqlf(*in.conn, "TRUNCATE {};", in.no_index_table);          // reset (untimed)
        it::exec_sql(*in.conn, "SET bioseq.update_index_on_insert = off;");  // load builds NO index
        it::exec_sql(*in.conn, "SET bioseq.read_sequence_index   = off;");   // align recomputes
        const std::string load_sql = fmt::format(
            "SELECT bioseq.load('{}'::regclass, '{}')", in.no_index_table, in.target_fasta);
        const std::string align_sql = fmt::format(
            "SELECT header, {} FROM {} WHERE mmsearch(seq, {}) ORDER BY header",
            proj, in.no_index_table, qlit);
        ScopedResourceMonitor resources;
        const auto start_ns = static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
        {
            auto _scope = bioseq::instr::region(kMeasuredRegion); // TIMED: load + recompute align
            it::exec_sql(*in.conn, load_sql);
            // A same-backend custom scan immediately after TRUNCATE + bioseq.load
            // can observe stale columnar descriptor state. Use a fresh backend for
            // the measured read; connection setup remains inside the timed region.
            it::connection_test_context read_conn;
            configure_bioseq_aligner_session(read_conn, in.kmer_size, in.window_size);
            configure_bioseq_scheduler_session(read_conn, pipeline_mode);
            it::exec_sql(read_conn, "SET bioseq.update_index_on_insert = off;");
            it::exec_sql(read_conn, "SET bioseq.read_sequence_index   = off;");
            parse_bioseq_rows(it::query_rows(read_conn, align_sql), in, out);
        }
        const auto end_ns = static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
        attach_resource_metrics(out, start_ns, end_ns, resources.stop());
        require_mapped(algo, in, out);
        log_measured_case(algo, pipeline_mode, minimap_threads, in, out);

    } else if (algo == "bioseq_load_only") {
        configure_bioseq_scheduler_session(*in.conn, pipeline_mode);
        it::exec_sqlf(*in.conn, "TRUNCATE {};", in.no_index_table);
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
        log_measured_case(algo, pipeline_mode, minimap_threads, in, out);

    } else if (algo == "bioseq_recompute_only") {
        const bool recompute_async_persist =
            param_string(params, "recompute_async_persist", "off") == "on";
        configure_bioseq_scheduler_session(*in.conn, pipeline_mode);
        it::exec_sqlf(*in.conn, "TRUNCATE {};", in.no_index_table);
        it::exec_sql(*in.conn, "SET bioseq.update_index_on_insert = off;");
        it::exec_sql(*in.conn, "SET bioseq.read_sequence_index   = off;");
        it::exec_sqlf(*in.conn,
            "SELECT bioseq.load('{}'::regclass, '{}')",
            in.no_index_table, in.target_fasta);

        it::connection_test_context read_conn;
        configure_bioseq_aligner_session(read_conn, in.kmer_size, in.window_size);
        configure_bioseq_scheduler_session(read_conn, pipeline_mode);
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
        }
        const auto end_ns = static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
        attach_resource_metrics(out, start_ns, end_ns, resources.stop());
        require_mapped(algo, in, out);
        log_measured_case(algo, pipeline_mode, minimap_threads, in, out);

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
        loge("[bench_e2e] unknown algorithm '{}'", algo);
    }
    return out;
}

// ---- §10.4 on_evaluate ----
[[nodiscard]] std::map<std::string, double> e2e_eval(
    std::string_view, const ParamMap&, const E2eInput&, const E2eOutput& o) {
    return {
        {"match_count",             static_cast<double>(o.match_count)},
        {"found_fraction",          o.found ? 1.0 : 0.0},
        {"mean_position_offset_bp", o.abs_offset_bp},
        {"measured_region_ns_manual", static_cast<double>(o.measured_region_ns)},
        {"peak_postgres_rss_mb",    static_cast<double>(o.peak_postgres_rss_kb) / 1024.0},
        {"peak_minimap2_rss_mb",    static_cast<double>(o.peak_minimap2_rss_kb) / 1024.0},
        {"peak_bench_rss_mb",       static_cast<double>(o.peak_bench_rss_kb) / 1024.0},
        {"min_tmp_available_gib",   static_cast<double>(o.min_tmp_available_kb) / (1024.0 * 1024.0)},
        {"peak_swap_used_mb",       o.peak_swap_used_mb},
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
