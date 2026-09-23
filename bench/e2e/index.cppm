/**
 * @file bench/e2e/index.cppm
 * @brief End-to-end SQL and minimap2 benchmark driver.
 *
 * Bioseq runs through `bioseq.load` and `mmsearch` over libpq; minimap2 runs
 * through its direct-spawn helper against signed-32-bit-safe FASTA shards.
 * Persisted index setup is untimed; no-index construction is part of the
 * framework outer E2E interval.
 */
module;

#include <simdjson.h>
#include <nlohmann/json.hpp>
#include <libpq-fe.h>
#include <algorithm>
#include <atomic>
#include <array>
#include <charconv>
#include <chrono>
#include <cerrno>
#include <cmath>
#include <compare>
#include <concepts>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <fstream>
#include <fcntl.h>
#include <functional>
#include <initializer_list>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <queue>
#include <set>
#include <signal.h>
#include <spawn.h>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/resource.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <system_error>
#include <thread>
#include <time.h>
#include <tuple>
#include <type_traits>
#include <variant>
#include <vector>
#include <unistd.h>

extern char** environ;

export module bioseq.bench.e2e;

import bioseq.common.logging;
import bioseq.common.hashing;
import bioseq.bench.framework;
import bioseq.instrumentation;
import bioseq.tests.integration.support;
import bioseq.common.seqan_adapter;
import bioseq.sequences;
import bioseq.sequences.generation;
import bioseq.io.writer;
import bioseq.io.reader;
import bioseq.io.options;
import bioseq.indexing.type;
import bioseq.indexing.storage;
import bioseq.indexing.minimizer_hashmap;
import bioseq.sequences.simd;
import bioseq.sequences.kmers.highway_minimizer_contract;
import bioseq.sequences.kmers.minimizer_layout_geometry;
import bioseq.chaining.core;
import bioseq.pipeline.pipe;
import bioseq.pipeline.runtime_contract;
import bioseq.pipeline.algorithm_identity_contract;

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
#error "BIOSEQ_E2E_MINIMAP2_BIN must name the built receipt helper"
#endif

namespace {
using bioseq::bench::framework::parameter_map;
using bioseq::bench::framework::run_constraints;
using bioseq::bench::framework::create_benchmark;
namespace gen = bioseq::sequences::generation;
// The E2E bench fixes only the storage width. Alphabet comes from the JSON
// test-case data (`dna4` or `dna5`) and is dispatched at compile time. uint64
// keeps generated ranges byte-identical to `bioseq.seq('<alphabet>', 64)` and to
// the IO writer's view, while the generic generator default remains uint32.
using bench_storage_t  = std::uint64_t;
template <sequence_alphabet p_alphabet>
using bench_config = sequence_custom_alphabet<p_alphabet, bench_storage_t>;
template <sequence_alphabet p_alphabet>
using bench_sequence_range = compact_sequence_range<bench_config<p_alphabet>>;
template <sequence_alphabet p_alphabet>
using bench_pair_t =
    typename gen::alphabet_sequence_generator<
        p_alphabet,
        bench_storage_t>::query_target_pair;

template <sequence_alphabet p_alphabet>
[[nodiscard]] constexpr std::string_view bench_alphabet_name() {
    if constexpr (std::same_as<p_alphabet, seq::dna4>) return "dna4";
    else if constexpr (std::same_as<p_alphabet, seq::dna5>) return "dna5";
    else return "?";
}
template <sequence_alphabet p_alphabet>
[[nodiscard]] inline std::string bench_seq_type() {
    return fmt::format(
        "bioseq.seq('{}', 64)",
        bench_alphabet_name<p_alphabet>());
}
namespace it = bioseq_int_tests;

enum class captured_query_status : std::uint8_t {
    tuples_ok,
    command_ok,
    server_error,
    connection_failure,
    unexpected_status,
};

struct captured_query_rows {
    it::rowset_t rows{};
    captured_query_status status = captured_query_status::unexpected_status;
    std::string result_status{};
    std::string connection_diagnostic{};
    std::string sqlstate{};
    std::string primary{};
    std::string detail{};
    std::string hint{};
};

using captured_pg_result =
    std::unique_ptr<PGresult, decltype(&PQclear)>;

/**
 * A libpq result whose row/error materialization is deliberately deferred
 * until after the measured SQL envelope.
 */
struct pending_captured_query_result {
    captured_pg_result result{nullptr, &PQclear};
};

[[nodiscard]] pending_captured_query_result execute_query_capturing_result(
        const it::connection_test_context& connection,
        const std::string& sql) {
    pending_captured_query_result pending;
    pending.result.reset(PQexec(connection.raw(), sql.c_str()));
    return pending;
}

[[nodiscard]] captured_query_rows materialize_captured_query_rows(
        const it::connection_test_context& connection,
        pending_captured_query_result pending) {
    auto& result = pending.result;
    const char* connection_message = PQerrorMessage(connection.raw());
    const std::string connection_diagnostic = connection_message == nullptr
        ? std::string{} : std::string{connection_message};
    if (result == nullptr) {
        return captured_query_rows{
            .status = PQstatus(connection.raw()) == CONNECTION_OK
                ? captured_query_status::unexpected_status
                : captured_query_status::connection_failure,
            .result_status = "null_result",
            .connection_diagnostic = connection_diagnostic,
        };
    }

    const ExecStatusType result_status = PQresultStatus(result.get());
    const auto field = [&result](const int code) {
        const char* value = PQresultErrorField(result.get(), code);
        return value == nullptr ? std::string{} : std::string{value};
    };
    const auto captured_status = [&]() noexcept {
        if (PQstatus(connection.raw()) != CONNECTION_OK) {
            return captured_query_status::connection_failure;
        }
        if (result_status == PGRES_TUPLES_OK) {
            return captured_query_status::tuples_ok;
        }
        if (result_status == PGRES_COMMAND_OK) {
            return captured_query_status::command_ok;
        }
        if (result_status == PGRES_FATAL_ERROR ||
            result_status == PGRES_NONFATAL_ERROR) {
            return captured_query_status::server_error;
        }
        return captured_query_status::unexpected_status;
    }();
    const char* result_status_name = PQresStatus(result_status);
    if (captured_status != captured_query_status::tuples_ok) {
        return captured_query_rows{
            .status = captured_status,
            .result_status = result_status_name == nullptr
                ? std::string{} : std::string{result_status_name},
            .connection_diagnostic = connection_diagnostic,
            .sqlstate = field(PG_DIAG_SQLSTATE),
            .primary = field(PG_DIAG_MESSAGE_PRIMARY),
            .detail = field(PG_DIAG_MESSAGE_DETAIL),
            .hint = field(PG_DIAG_MESSAGE_HINT),
        };
    }

    captured_query_rows captured;
    captured.status = captured_query_status::tuples_ok;
    captured.result_status = result_status_name == nullptr
        ? std::string{} : std::string{result_status_name};
    captured.connection_diagnostic = connection_diagnostic;
    const int row_count = PQntuples(result.get());
    const int column_count = PQnfields(result.get());
    captured.rows.reserve(static_cast<std::size_t>(row_count));
    for (int row = 0; row < row_count; ++row) {
        it::row_t values;
        values.reserve(static_cast<std::size_t>(column_count));
        for (int column = 0; column < column_count; ++column) {
            if (PQgetisnull(result.get(), row, column) != 0) {
                values.emplace_back(std::nullopt);
            } else {
                values.emplace_back(
                    std::string{PQgetvalue(result.get(), row, column)});
            }
        }
        captured.rows.push_back(std::move(values));
    }
    return captured;
}

// Matrix tuples include deliberately unsupported selections (notably partial
// Vulkan combinations).  The shared integration-test query helper asserts on
// every PostgreSQL error, which is correct for tests but would abort the whole
// benchmark matrix. Keep the non-asserting boundary local to this benchmark;
// the caller serializes only exact tuple-specific server rejections and treats
// every transport, protocol, schema, or unrecognized server failure as fatal.
[[nodiscard]] captured_query_rows query_rows_capturing_error(
        const it::connection_test_context& connection,
        std::string_view sql) {
    const std::string owned_sql{sql};
    return materialize_captured_query_rows(
        connection,
        execute_query_capturing_result(connection, owned_sql));
}

// The single measured region (registered once at static init).
const auto* const k_measured_region =
    bioseq::instrumentation::register_static_region("e2e_measured");

// ---- named constants (no scattered magic literals — MEDIUM-05) ----
// minimap2 stores each parsed sequence length in a SIGNED int and asserts
// `l_seq <= INT32_MAX` (bench/tool_minimap2/minimap2/bseq.c:97, bseq.h:15). The
// real per-record ceiling is therefore 2^31-1, NOT the mm_idx_seq_t::len uint32
// 2^32 the earlier draft claimed (HIGH-08).
constexpr std::int64_t  k_minimap2_max_record_bp = 2'147'483'647;            // INT32_MAX
constexpr std::int64_t  k_minimap2_split_bp     = 2'000'000'352;            // < ceiling
static_assert(k_minimap2_split_bp <= k_minimap2_max_record_bp,
              "minimap2 split records must not exceed INT32_MAX bases");
using dna4_u64_traits = compact_sequence_range<
    sequence_custom_alphabet<seq::dna4, bench_storage_t>>::traits;
using dna5_u64_traits = compact_sequence_range<
    sequence_custom_alphabet<seq::dna5, bench_storage_t>>::traits;
constexpr std::size_t k_bench_split_alignment_bp =
    std::lcm(dna4_u64_traits::letters_per_unit, dna5_u64_traits::letters_per_unit);
static_assert(k_minimap2_split_bp % static_cast<std::int64_t>(k_bench_split_alignment_bp) == 0,
              "split must be unit-aligned for every benchmark alphabet");
constexpr std::int64_t  k_default_reference_length_bp        = 2'000'000'000;
constexpr std::int64_t  k_per_seq_index_preflight_threshold_bp = 4'000'000'000LL;
constexpr int           k_default_kmer_size    = 15;
constexpr int           k_default_window_size  = 10;
// Similar mappings are accepted only when their coordinate delta is strictly
// below 15 bp. A 15 bp delta itself is a failure.
constexpr std::int64_t  k_position_tolerance_bp = 15;
constexpr std::int64_t  k_max_mapping_quality = 60;
constexpr std::size_t   k_popen_buf_bytes      = std::size_t{1} << 16;
constexpr std::size_t   k_minimap2_stdout_max_bytes = std::size_t{1} << 28;
constexpr std::size_t   k_minimap2_stderr_max_bytes = std::size_t{1} << 23;
constexpr std::size_t   k_minimap2_receipt_max_bytes = std::size_t{1} << 27;
constexpr int           k_minimap2_receipt_child_fd = 198;
constexpr auto          k_minimap2_child_timeout = std::chrono::hours{12};
constexpr auto          k_minimap2_terminate_grace = std::chrono::seconds{5};
constexpr std::uint64_t  k_e2e_builder_guard_ms = 14'400'000;
constexpr std::uint64_t  k_e2e_timeout_cleanup_margin_ms = 600'000;
constexpr std::uint64_t  k_e2e_max_statement_timeout_ms =
    k_e2e_builder_guard_ms - k_e2e_timeout_cleanup_margin_ms;
#if defined(__APPLE__)
// mach_absolute_time (the framework clock) and CLOCK_MONOTONIC (the immutable
// helper ABI) are nanosecond views of the same kernel monotonic tick rate on
// the local campaign platform. Their epochs need not, and must not, be compared.
constexpr std::string_view k_minimap2_framework_duration_rate_contract =
    "same_kernel_monotonic_nanosecond_rate";
#elif defined(__linux__) && defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
// The dedicated Linux E2E module clock and the immutable helper both call
// CLOCK_MONOTONIC. Their epochs are still not compared; only durations are.
static_assert(
    bioseq::instrumentation::clock::now_ns_clock_id == CLOCK_MONOTONIC,
    "Linux E2E framework clock must match the immutable minimap2 receipt clock");
constexpr std::string_view k_minimap2_framework_duration_rate_contract =
    "same_kernel_monotonic_nanosecond_rate";
#else
// Ordinary Linux uses CLOCK_MONOTONIC_RAW while the immutable helper declares
// CLOCK_MONOTONIC. Other platform pairs are likewise not proven here.
constexpr std::string_view k_minimap2_framework_duration_rate_contract =
    "not_proven";
#endif
constexpr std::size_t   k_fasta_stream_chunk_bp = std::size_t{1} << 24;      // bounded writer body slice
// Bounded FASTA lines keep every reader alphabet/storage path within its input
// slab; some fallback encoders must buffer one complete body line.
constexpr std::size_t   k_fasta_line_width     = std::size_t{1} << 16;
constexpr std::string_view k_minimizers_per_fragment_index =
    bioseq::indexing::k_minimizers_per_fragment_index_name;
constexpr std::string_view k_minimizers_per_sequence_index =
    bioseq::indexing::k_minimizers_per_sequence_index_name;
/** @brief Backend family whose indexes must be prepared during setup. */
enum class just_index_setup_kind {
    scalar,
    simd,
    gpu,
};

/** @brief Index-building modes requested by one expanded benchmark tuple. */
struct e2e_setup_selection {
    bool scalar_just_index = false;
    bool simd_just_index = false;
    bool gpu_just_index = false;
    bool minimap2_just_index = false;
};

}  // namespace

#if defined(BIOSEQ_E2E_TESTING_API)
export namespace bioseq::bench::e2e::detail {
#else
namespace bioseq::bench::e2e::detail {
#endif

inline constexpr std::string_view sliced_no_index_algorithm =
    "bioseq_block_simd_dp_two_stack_nthash_no_index_sliced";
inline constexpr std::string_view algorithm_matrix_just_index =
    "bioseq_algorithm_matrix_just_index";
inline constexpr std::string_view algorithm_matrix_no_index =
    "bioseq_algorithm_matrix_no_index";

/** @brief Concrete persisted-index variants needed by one benchmark run. */
struct e2e_just_index_setup_requirements {
    bool scalar_per_fragment = false;
    bool scalar_per_sequence = false;
    bool simd_per_fragment = false;
    bool simd_per_sequence = false;
    bool gpu_per_fragment = false;
    bool gpu_per_sequence = false;
    bool minimap2 = false;
    struct matrix_setup {
        std::string minimizer_algorithm;
        std::string chaining_algorithm;
        std::string base_alignment_algorithm;
        std::string minimizer_index;
        std::string identity;
    };
    std::vector<matrix_setup> matrix_setups;

    [[nodiscard]] bool any_bioseq() const noexcept {
        return scalar_per_fragment || scalar_per_sequence
            || simd_per_fragment || simd_per_sequence
            || gpu_per_fragment || gpu_per_sequence
            || !matrix_setups.empty();
    }
};

/** @brief Expected mapping coordinates for one generated query. */
struct e2e_query_truth {
    std::int32_t id = 0;
    std::string bases;
    std::int64_t truth_target_start = 0;
    std::int64_t truth_target_end = 0;
    bool truth_is_reverse = false;
    std::int64_t target_window_start = 0;
    std::string target_window_bases;
};

/** @brief One ordered, lossless oracle observation for a submitted query. */
struct e2e_query_oracle_observation {
    std::int64_t query_id = -1;
    std::int64_t primary_match_count = 0;
    std::int64_t mapq = -1;

    auto operator<=>(const e2e_query_oracle_observation&) const = default;
};

/** One deterministic half-open physical slice of a logical query set. */
struct logical_query_chunk {
    std::size_t begin = 0;
    std::size_t end = 0;

    [[nodiscard]] std::size_t size() const noexcept { return end - begin; }
    auto operator<=>(const logical_query_chunk&) const = default;
};

[[nodiscard]] std::vector<logical_query_chunk> make_logical_query_chunks(
        const std::size_t query_count,
        const std::size_t chunk_size) {
    if (query_count == 0 || chunk_size == 0 || chunk_size > query_count) {
        throw std::invalid_argument(
            "logical E2E query chunk plan has an invalid cardinality");
    }
    std::vector<logical_query_chunk> chunks;
    chunks.reserve((query_count - 1u) / chunk_size + 1u);
    for (std::size_t begin = 0; begin < query_count; begin += chunk_size) {
        chunks.push_back(logical_query_chunk{
            .begin = begin,
            .end = std::min(query_count, begin + chunk_size),
        });
    }
    if (chunks.empty() || chunks.front().begin != 0
        || chunks.back().end != query_count) {
        throw std::logic_error(
            "logical E2E query chunk plan did not cover the full query set");
    }
    for (std::size_t index = 0; index < chunks.size(); ++index) {
        if (chunks[index].begin >= chunks[index].end
            || chunks[index].size() > chunk_size
            || (index != 0 && chunks[index - 1u].end != chunks[index].begin)) {
            throw std::logic_error(
                "logical E2E query chunk plan is not contiguous and ordered");
        }
    }
    return chunks;
}

/** @brief Provenance retained after one genuinely measured matrix rejection. */
struct e2e_matrix_attestation_rejection {
    std::size_t source_repeat = 0;
    std::string rejection_reason;
};

/** @brief Immutable setup artifacts consumed by each measured execution. */
struct e2e_input {
    // Connection is shared (const-usable; SQL mutates the DB, not this struct).
    std::shared_ptr<it::connection_test_context> conn;
    std::string alphabet;                        // "dna4" / "dna5"
    std::string seq_type;                        // bioseq.seq('<alphabet>', 64)
    std::string size_label;                      // "2gbp" / "4gbp"
    std::string just_index_table_per_fragment;   // loaded + per-fragment indexed in on_input
    std::string just_index_table_per_sequence;   // loaded + per-sequence indexed in on_input
    std::string just_index_table_simd_per_fragment;
    std::string just_index_table_simd_per_sequence;
    std::string just_index_table_scalar_per_fragment;
    std::string just_index_table_scalar_per_sequence;
    std::string just_index_table_gpu_per_fragment;
    std::string just_index_table_gpu_per_sequence;
    std::map<std::string, std::string> matrix_just_index_tables;
    // Setup-only ownership for one validated observed-rung target.  Cleanup
    // preserves it only after a READY source row was captured successfully.
    std::string matrix_just_index_target_to_retain;
    // Exact untimed bioseq.update_index capability rejections, keyed by the
    // minimizer-producer/index identity which owns persisted setup.
    std::map<std::string, captured_query_rows> matrix_just_index_setup_rejections;
    // Exact untimed bioseq.seq typmod capability rejection for the shared
    // algorithm-matrix no-index table. No table or pipeline work follows it.
    std::optional<captured_query_rows> matrix_no_index_setup_rejection;
    // One complete post-measurement SQL/EXPLAIN/control qualification per
    // exact algorithm-matrix cell.  Framework batches execute sequentially
    // against this const-visible Input; separate groups own separate Inputs.
    // A value is inserted only after every detached qualification guard has
    // succeeded, and it always retains the source repeat's real SQL-session
    // identity rather than claiming to describe a later cold repeat.
    mutable std::map<std::string, nlohmann::json>
        matrix_gate8_cell_qualifications;
    // A measured attestation failure makes later repeats of only that exact
    // resolved cell timing-ineligible.  This process-local cache is never
    // populated by setup/SQL/correctness failures and cannot cross Inputs,
    // runs, or binaries.
    mutable std::map<std::string, e2e_matrix_attestation_rejection>
        matrix_measured_attestation_rejections;
    // Durable, non-index-maintained query relation used only by non-paired
    // Bioseq algorithm-matrix rows.  Keeping it outside the per-repeat
    // measurement boundary makes A exactly the one mmsearch statement while
    // preserving the paired/chunk temporary-table recipe.
    std::string matrix_query_table;
    // The BIOSEQ table AM owns this identity.  It is not a zero-based ordinal:
    // capture it from the loaded row's ctid and carry that exact query key into
    // every READY-generation inspection before and after setup.
    std::map<std::string, std::int64_t> just_index_sequence_rowno_by_table;
    std::string no_index_table;                  // base target loaded untimed; no minimizer sidecar
    std::string target_fasta;                    // single-record, for bioseq.load
    std::string target_fasta_split;              // <=split_bp records, for minimap2
    std::string query_fasta;                     // for minimap2
    std::vector<std::string> query_chunk_fastas; // canonical contiguous qid chunks
    std::string mmi_path;                        // prebuilt minimap2 index
    std::uint64_t minimap2_index_parts = 1;       // receipt-validated index partition count
    std::vector<e2e_query_truth> queries;
    bool paired_minimap2_oracle = false;
    // Derived from the expanded comparison rows, not a user-facing switch.
    // Legacy Bioseq comparators must retain the same timing scope as minimap2.
    bool algorithm_matrix_measurement_scope = false;
    std::size_t logical_query_chunk_size = 0;
    std::uint64_t statement_timeout_ms = 0;
    std::string minimap2_setup_receipt_checksum;
    std::string minimap2_setup_receipt_artifact_path;
    std::string minimap2_setup_receipt_artifact_digest;
    nlohmann::json minimap2_setup_receipt_summary = nullptr;
    std::string paired_generated_target_content_crc64;
    std::string paired_persisted_target_content_crc64;
    bool require_nice_attestation = false;
    std::int64_t priority_target_nice = -1;
    std::int64_t runner_pid = -1;
    std::int64_t runner_nice = -1;
    int kmer_size = 15, window_size = 10;
    bool cigar_projection = false;
    bool mapq_projection = false;
    bool prefilter_enabled = false;
    bool designed_indel_cigar = false;
    bool designed_many_gap_cigar = false;
    bool require_cigar_indel = false;
    std::uint32_t min_cigar_deletion_ops = 0;
    std::uint32_t min_cigar_insertion_ops = 0;
    // PAF absolute-coordinate remap (§11). Keep this type self-contained for
    // the target-owned testing interface rather than exposing implementation
    // constants from the primary module.
    std::size_t minimap2_split_bp = std::size_t{2'000'000'352};
    std::int64_t truth_target_start = 0;
    bool truth_is_reverse = false;
    std::int64_t target_bp = 0;
};

[[nodiscard]] constexpr bool is_sliced_pipeline_stats_configuration_valid(
        std::string_view algorithm,
        const bool cigar_projection) noexcept {
    return algorithm != sliced_no_index_algorithm || cigar_projection;
}

/** @brief Stats that a cold no-index table has no persisted index state. */
struct cold_no_index_policy_absence_stats {
    std::int64_t target_rows = -1;
    std::int64_t target_cardinality_metadata_only = 0;
    std::int64_t source_parent_rows_read = -1;
    std::int64_t source_fragment_rows_read = -1;
    std::int64_t source_decoded_bp = -1;
    std::int64_t physical_index_relations = -1;
    std::int64_t persisted_index_rows = -1;
    std::int64_t generation_objects = -1;
};

/** @brief Measurements and correctness stats emitted by one execution. */
struct e2e_output {
    static constexpr std::size_t k_alignment_snapshot_column_count = 151;
    static constexpr std::size_t k_alignment_snapshot_row_count = 20;
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
    std::int64_t mapq_result_count = 0;
    std::vector<e2e_query_oracle_observation> ordered_query_oracle;
    std::array<std::uint32_t, 8> ordered_query_oracle_sha256_words{};
    std::string ordered_query_oracle_sha256;
    std::array<std::uint32_t, 8> ordered_query_mapq_sha256_words{};
    std::string ordered_query_mapq_sha256;
    std::int64_t ordered_query_oracle_available = 0;
    std::int64_t paired_oracle_repeat_ordinal = -1;
    std::int64_t paired_oracle_comparison_available = 0;
    std::int64_t paired_oracle_exact = -1;
    std::string paired_oracle_diagnostic;
    std::int64_t paired_mapq_vector_equal = -1;
    std::string paired_mapq_diagnostic;
    std::int64_t paired_oracle_verdict_sealed = 0;
    std::string paired_generated_target_content_crc64;
    std::string paired_persisted_target_content_crc64;
    std::int64_t logical_chunking_enabled = 0;
    std::int64_t logical_chunk_count = 0;
    std::int64_t logical_chunk_size = 0;
    std::int64_t logical_statement_timeout_ms = 0;
    std::int64_t logical_statement_timeout_restored = -1;
    std::uint64_t validation_excluded_ns = 0;
    nlohmann::json logical_chunk_receipts = nullptr;
    std::int64_t priority_target_nice = -1;
    std::int64_t runner_pid = -1;
    std::int64_t runner_nice = -1;
    std::int64_t postgres_backend_nice = -1;
    std::int64_t priority_attestation_accepted = -1;
    double abs_offset_bp = 0.0;
    double max_abs_offset_bp = 0.0;
    bool position_offset_available = false;
    std::string requested_backend;
    std::string actual_backend;
    std::string requested_target_index_source;
    std::string actual_target_index_source;
    std::string requested_simd_chaining_policy;
    std::string observed_simd_chaining_policy;
    std::string requested_simd_alignment_algorithm;
    std::string observed_simd_alignment_algorithm;
    std::string attested_requested_minimizer_algorithm;
    std::string observed_minimizer_algorithm;
    std::string attested_requested_chaining_algorithm;
    std::string observed_chaining_algorithm;
    std::string attested_requested_base_alignment_algorithm;
    std::string observed_base_alignment_algorithm;
    std::int64_t simd_stats_schema_valid = -1;
    std::int64_t nthash_vector_batches = -1;
    std::int64_t nthash_resident_vector_stream_kernel_calls = -1;
    std::int64_t simd_chaining_exact_policy_calls = -1;
    std::int64_t simd_chaining_bounded_policy_calls = -1;
    std::int64_t simd_chaining_bounded_score_batches = -1;
    std::int64_t simd_chaining_bounded_cutoff_stops = -1;
    std::int64_t simd_chaining_bounded_candidates_skipped = -1;
    std::int64_t simd_chaining_policy_rejections = -1;
    std::int64_t simd_chaining_policy_stats_saturated = -1;
    std::int64_t block_aligner_execution_stats_available = -1;
    std::int64_t block_aligner_execution_stats_consistent = -1;
    std::int64_t block_aligner_executed_calls = -1;
    std::int64_t block_aligner_nonempty_calls = -1;
    std::int64_t block_aligner_nonempty_calls_with_simd = -1;
    std::int64_t block_aligner_simd_dp_vector_batches = -1;
    std::int64_t block_aligner_simd_dp_cells = -1;
    std::int64_t block_aligner_forbidden_ksw2_calls = -1;
    std::int64_t block_aligner_forbidden_scalar_biological_fallbacks = -1;
    std::int64_t block_aligner_execution_stats_saturated = -1;
    std::int64_t prefilter_execution_stats_available = -1;
    std::int64_t prefilter_activity_count = -1;
    std::int64_t prefilter_execution_stats_saturated = -1;
    nlohmann::json prefilter_execution_stats = nullptr;
    std::int64_t pipeline_stats_available = 0;
    std::int64_t pipeline_stats_attached_to_measurement = 0;
    std::int64_t pipeline_stats_consistent = 0;
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
    std::string persisted_layout;
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
    std::int64_t grouped_soa_reader_capacity_accounting_passes = -1;
    std::int64_t grouped_soa_reader_global_position_storage_preparations = -1;
    std::int64_t grouped_soa_reader_global_position_storage_allocations = -1;
    std::int64_t grouped_soa_reader_position_append_growth_reservations = -1;
    std::int64_t grouped_soa_reader_retained_position_count = -1;
    std::int64_t grouped_soa_reader_retained_position_capacity = -1;
    std::int64_t grouped_soa_reader_directory_capacity_bytes = -1;
    std::int64_t grouped_soa_reader_query_hash_capacity_bytes = -1;
    std::int64_t grouped_soa_reader_query_record_capacity_bytes = -1;
    std::int64_t grouped_soa_reader_ordinal_route_capacity_bytes = -1;
    std::int64_t grouped_soa_reader_selected_extent_capacity_bytes = -1;
    std::int64_t grouped_soa_reader_current_selected_control_capacity_bytes = -1;
    std::int64_t grouped_soa_reader_global_position_capacity_bytes = -1;
    std::int64_t grouped_soa_reader_control_payload_capacity_bytes = -1;
    std::int64_t grouped_soa_reader_owned_capacity_bytes = -1;
    std::int64_t grouped_soa_reader_admitted_bytes = -1;
    std::int64_t grouped_soa_reader_exact_position_image_checks = -1;
    std::int64_t grouped_soa_reader_exact_position_image_failures = -1;
    std::int64_t grouped_soa_reader_retained_layout_checks = -1;
    std::int64_t grouped_soa_reader_retained_layout_failures = -1;
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
    std::int64_t fragment_native_peak_block_stats_capacity_bytes = -1;
    std::int64_t fragment_native_occurrence_allocations = -1;
    std::int64_t fragment_native_query_plan_allocations = -1;
    std::int64_t fragment_native_query_group_offset_allocations = -1;
    std::int64_t fragment_native_query_membership_filter_allocations = -1;
    std::int64_t fragment_native_query_radix_allocations = -1;
    std::int64_t fragment_native_occurrence_count_allocations = -1;
    std::int64_t fragment_native_block_layout_allocations = -1;
    std::int64_t fragment_native_block_stats_allocations = -1;
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
    std::int64_t are_fragment_native_stats_saturated = -1;
    std::int64_t are_grouped_soa_reader_memory_stats_saturated = -1;
    nlohmann::json minimizer_fragment_reader_stats = nullptr;
    std::int64_t hash_major_directory_rows = -1;
    std::int64_t hash_major_directory_bytes = -1;
    std::int64_t hash_major_control_rows = -1;
    std::int64_t hash_major_control_bytes = -1;
    std::int64_t hash_major_posting_rows = -1;
    std::int64_t hash_major_posting_bytes = -1;
    std::int64_t hash_major_posting_ranges_delivered = -1;
    std::int64_t hash_major_selected_block_count = -1;
    std::int64_t hash_major_accepted_hash_count = -1;
    std::int64_t hash_major_retained_position_count = -1;
    std::int64_t hash_major_authorized_posting_bytes = -1;
    std::int64_t hash_major_peak_admitted_bytes = -1;
    std::int64_t hash_major_retained_owner_bytes = -1;
    std::int64_t hash_major_admitted_bytes = -1;
    std::int64_t hash_major_failures = -1;
    std::int64_t hash_major_is_saturated = -1;
    nlohmann::json hash_major_index_reader_stats = nullptr;
    std::int64_t persisted_gpu_operation_stats_schema_valid = -1;
    std::int64_t persisted_gpu_operation_stats_accepted = -1;
    std::int64_t persisted_gpu_operation_stats_complete = -1;
    std::int64_t persisted_gpu_operation_stats_saturated = -1;
    std::int64_t persisted_gpu_dropped_operation_count = -1;
    std::int64_t persisted_gpu_operation_count = -1;
    std::int64_t persisted_gpu_accepted_operation_count = -1;
    std::int64_t persisted_gpu_job_count = -1;
    std::int64_t persisted_gpu_accepted_job_count = -1;
    nlohmann::json persisted_gpu_operation_stats = nullptr;
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
    bool measured_region_available = false;
    std::int64_t downstream_active_wall_ns = -1;
    std::int64_t downstream_active_wall_invocation_count = -1;
    std::int64_t downstream_active_wall_unavailable_invocation_count = -1;
    std::int64_t downstream_active_wall_available = -1;
    std::int64_t downstream_work_ns = -1;
    std::int64_t downstream_work_saturated = -1;
    std::int64_t benchmark_e2e_interval_schema_revision = -1;
    std::int64_t benchmark_e2e_interval_available = -1;
    std::int64_t benchmark_e2e_interval_union_ns = -1;
    std::int64_t benchmark_e2e_interval_count = -1;
    std::int64_t benchmark_e2e_cigar_interval_count = -1;
    std::int64_t benchmark_e2e_mapq_interval_count = -1;
    std::int64_t benchmark_e2e_interval_finalization_count = -1;
    std::string benchmark_e2e_interval_failure_code;
    nlohmann::json benchmark_e2e_interval_origins = nullptr;
    std::int64_t minimap2_receipt_available = -1;
    std::int64_t minimap2_receipt_pid = -1;
    std::int64_t minimap2_cigar_interval_count = -1;
    std::int64_t minimap2_mapq_interval_count = -1;
    std::int64_t minimap2_cigar_union_ns = -1;
    std::int64_t minimap2_mapq_union_ns = -1;
    std::int64_t minimap2_cigar_or_mapq_union_ns = -1;
    std::string minimap2_receipt_checksum;
    std::string minimap2_receipt_artifact_path;
    std::string minimap2_receipt_artifact_digest;
    nlohmann::json minimap2_receipt_summary = nullptr;
    std::int64_t mapq_execution_available = -1;
    std::int64_t mapq_executed = -1;
    std::int64_t mapq_execution_conflict = -1;
    std::int64_t tuple_accepted = 1;
    std::string cell_disposition = "accepted";
    std::string phase = "completed";
    std::string tuple_rejection_reason;
    std::int64_t prior_attestation_rejection_source_repeat = -1;
    std::string prior_attestation_rejection_reason;
    std::string behavior_difference_comment;
    std::string sql_result_status;
    std::string sqlstate;
    std::string sql_primary;
    std::string sql_detail;
    std::string sql_hint;
    std::string sql_connection_diagnostic;
    nlohmann::json measured_query_connection_failure_proof = nullptr;
    // Untimed DOC0433 Gate-8 SQL/EXPLAIN and control evidence.  The raw
    // serializer only copies this already-complete value; it performs no SQL.
    nlohmann::json gate8_sql_proof = nullptr;
    std::int64_t correctness_evaluated = 0;
    std::string requested_minimizer_algorithm;
    std::string requested_chaining_algorithm;
    std::string requested_base_alignment_algorithm;
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
    std::string pre_restart_persisted_layout;
    std::string post_restart_persisted_layout;
    nlohmann::json pre_restart_hash_major_ready_stats = nullptr;
    nlohmann::json post_restart_hash_major_ready_stats = nullptr;
    std::int64_t format6_restart_audit_accepted = -1;
    // Dedicated no-index lifecycle stats.  Do not overload the persisted
    // just_index fields: no_index validates an untimed base-data load with no
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
    std::int64_t cold_no_index_pre_restart_minimizer_fragment_rows = -1;
    std::int64_t cold_no_index_pre_restart_minimizer_sequence_rows = -1;
    std::int64_t cold_no_index_post_restart_minimizer_fragment_rows = -1;
    std::int64_t cold_no_index_post_restart_minimizer_sequence_rows = -1;
    std::int64_t cold_no_index_pre_restart_async_jobs = -1;
    std::int64_t cold_no_index_post_restart_async_jobs = -1;
    std::int64_t cold_no_index_measured_async_jobs = -1;
    std::int64_t cold_no_index_pre_restart_gucs_off = -1;
    std::int64_t cold_no_index_post_restart_gucs_off = -1;
    std::int64_t cold_no_index_measured_gucs_off = -1;
    cold_no_index_policy_absence_stats cold_no_index_pre_restart_minimizer_fragment{};
    cold_no_index_policy_absence_stats cold_no_index_pre_restart_minimizer_sequence{};
    cold_no_index_policy_absence_stats cold_no_index_post_restart_minimizer_fragment{};
    cold_no_index_policy_absence_stats cold_no_index_post_restart_minimizer_sequence{};
    cold_no_index_policy_absence_stats cold_no_index_measured_minimizer_fragment{};
    cold_no_index_policy_absence_stats cold_no_index_measured_minimizer_sequence{};
    std::int64_t cold_no_index_lifecycle_accepted = 0;
    std::int64_t persisted_policy_audit_accepted = -1;
    std::int64_t audit_selected_minimizer_is_sequence = -1;
    std::int64_t audit_minimizer_fragment_rows = -1;
    std::int64_t audit_minimizer_sequence_rows = -1;
    std::int64_t audit_minimizer_fragment_update_on_insert = -1;
    std::int64_t audit_minimizer_sequence_update_on_insert = -1;
    std::int64_t audit_minimizer_fragment_active_read = -1;
    std::int64_t audit_minimizer_sequence_active_read = -1;
    std::int64_t audit_async_persist_jobs_waiting = -1;
    std::int64_t audit_index_rows_wait_attempts = 0;
    std::int64_t hash_major_build_backend_pid = -1;
    std::int64_t hash_major_build_db_oid = -1;
    std::int64_t hash_major_build_relfilenode = -1;
    std::int64_t hash_major_build_sequence_rowno = -1;
    std::int64_t hash_major_build_total_run_count = -1;
    std::int64_t hash_major_build_resident_runs = -1;
    std::int64_t hash_major_build_file_runs = -1;
    std::int64_t hash_major_build_resident_run_bytes = -1;
    std::int64_t hash_major_build_resident_peak_bytes = -1;
    std::int64_t hash_major_build_spilled_run_bytes = -1;
    std::int64_t hash_major_build_postgres_temp_file_bytes = -1;
    std::int64_t hash_major_build_postgres_temp_file_count = -1;
    std::int64_t hash_major_build_merge_reader_buffer_bytes = -1;
    std::int64_t hash_major_build_merge_pass_count = -1;
    std::int64_t hash_major_build_spill_count_from_pressure = -1;
    std::int64_t hash_major_build_resident_demotions = -1;
    std::int64_t hash_major_build_position_spill_bytes = -1;
    std::int64_t hash_major_build_position_spill_count = -1;
    std::int64_t hash_major_build_shared_generation = -1;
    std::int64_t hash_major_build_worker_build_ns = -1;
    std::int64_t hash_major_build_pipeline_mode = -1;
    std::int64_t hash_major_build_max_concurrency = -1;
    std::int64_t hash_major_build_index_memory_cap_mb = -1;
    std::int64_t hash_major_build_lazy_worker_shareable = -1;
    std::array<std::int64_t, k_alignment_snapshot_column_count>
        alignment_snapshot_numeric_columns = [] {
            std::array<std::int64_t, k_alignment_snapshot_column_count> values{};
            values.fill(-1);
            return values;
        }();
    // Lossless copy of the exact same-invocation SRF result.  The selected
    // scalar fields above drive fail-closed acceptance; this complete copy is
    // the durable stats written to report.json so later analysis never has
    // to trust a lossy projection of the stats.
    it::rowset_t alignment_snapshot_rows{};

    /**
     * The schema-valid bit is assigned only after capture has validated the
     * exact revision-3 object, including null cache validation, an empty
     * operations array, and zero complete-operation count for every route
     * other than the legacy persisted all-GPU pipeline.  Reuse that capture
     * proof here instead of reparsing JSON in normal benchmark operation.
     */
    [[nodiscard]] bool has_exact_empty_persisted_gpu_operation_ledger()
            const noexcept {
        return persisted_gpu_operation_stats_schema_valid == 1
            && persisted_gpu_operation_stats_accepted == 0
            && persisted_gpu_operation_stats_complete == 1
            && persisted_gpu_operation_stats_saturated == 0
            && persisted_gpu_dropped_operation_count == 0
            && persisted_gpu_operation_count == 0
            && persisted_gpu_accepted_operation_count == 0
            && persisted_gpu_job_count == 0
            && persisted_gpu_accepted_job_count == 0;
    }

    /** Preserve the existing positive ledger proof for legacy all-GPU runs. */
    [[nodiscard]] bool has_accepted_legacy_persisted_gpu_operation_ledger(
            const std::int64_t expected_job_count) const noexcept {
        return persisted_gpu_operation_stats_schema_valid == 1
            && persisted_gpu_operation_stats_accepted == 1
            && persisted_gpu_operation_stats_complete == 1
            && persisted_gpu_operation_stats_saturated == 0
            && persisted_gpu_dropped_operation_count == 0
            && persisted_gpu_operation_count > 0
            && persisted_gpu_accepted_operation_count
                == persisted_gpu_operation_count
            && persisted_gpu_job_count == expected_job_count
            && persisted_gpu_accepted_job_count == persisted_gpu_job_count;
    }
};

}  // namespace bioseq::bench::e2e::detail

namespace {

using bioseq::bench::e2e::detail::cold_no_index_policy_absence_stats;
using bioseq::bench::e2e::detail::e2e_input;
using bioseq::bench::e2e::detail::e2e_matrix_attestation_rejection;
using bioseq::bench::e2e::detail::e2e_just_index_setup_requirements;
using bioseq::bench::e2e::detail::e2e_output;
using bioseq::bench::e2e::detail::e2e_query_oracle_observation;
using bioseq::bench::e2e::detail::e2e_query_truth;
using bioseq::bench::e2e::detail::is_sliced_pipeline_stats_configuration_valid;
using bioseq::bench::e2e::detail::logical_query_chunk;
using bioseq::bench::e2e::detail::make_logical_query_chunks;

[[nodiscard]] bool e2e_expects_hash_major(
        const e2e_input& in,
        std::string_view minimizer_index,
        bool gpu_seed_selected) noexcept;
[[nodiscard]] nlohmann::json capture_hash_major_ready_stats(
    const it::connection_test_context& connection,
    const std::string& table,
    std::string_view minimizer_index,
    std::int64_t sequence_rowno,
    const e2e_input& in,
    std::string_view requested_minimizer_algorithm);

[[nodiscard]] std::optional<just_index_setup_kind>
e2e_just_index_setup_kind(std::string_view name) {
    if (name == "bioseq_ksw2_scalar_dp_scalar_mm_just_index") {
        return just_index_setup_kind::scalar;
    }
    if (name == "bioseq_just_index"
        || name == "bioseq_block_simd_dp_two_stack_nthash_just_index") {
        return just_index_setup_kind::simd;
    }
    if (name == "bioseq_block_vulkan_resident_gpu_index_frontend_just_index"
               || name == "bioseq_block_gpu_dp_gpu_seed_just_index") {
        return just_index_setup_kind::gpu;
    }
    return std::nullopt;
}

[[nodiscard]] e2e_just_index_setup_requirements
resolve_e2e_just_index_setup_requirements(
        const run_constraints& constraints) {
    e2e_just_index_setup_requirements requirements{};
    for (const auto& tuple : constraints.execution_tuples) {
        if (tuple.algorithm_name == "minimap2_just_index") {
            requirements.minimap2 = true;
            continue;
        }
        if (tuple.algorithm_name ==
            bioseq::bench::e2e::detail::algorithm_matrix_just_index) {
            const auto string_parameter = [&tuple](std::string_view name) -> std::string {
                const auto found = tuple.params.find(std::string{name});
                if (found == tuple.params.end() || !found->second.is_string()) {
                    throw std::invalid_argument(fmt::format(
                        "algorithm-matrix tuple has no resolved string '{}' parameter", name));
                }
                return std::string{found->second.as_string()};
            };
            e2e_just_index_setup_requirements::matrix_setup setup{
                .minimizer_algorithm = string_parameter("minimizer_algorithm"),
                .chaining_algorithm = string_parameter("chaining_algorithm"),
                .base_alignment_algorithm = string_parameter("base_alignment_algorithm"),
                .minimizer_index = string_parameter("minimizer_index"),
            };
            setup.identity = fmt::format("{}|{}",
                setup.minimizer_algorithm, setup.minimizer_index);
            if (std::ranges::none_of(requirements.matrix_setups,
                    [&setup](const auto& existing) { return existing.identity == setup.identity; })) {
                requirements.matrix_setups.push_back(std::move(setup));
            }
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
        if (policy == k_minimizers_per_fragment_index) {
            switch (*setup_kind) {
                case just_index_setup_kind::scalar:
                    required = &requirements.scalar_per_fragment;
                    break;
                case just_index_setup_kind::simd:
                    required = &requirements.simd_per_fragment;
                    break;
                case just_index_setup_kind::gpu:
                    required = &requirements.gpu_per_fragment;
                    break;
            }
        } else if (policy == k_minimizers_per_sequence_index) {
            switch (*setup_kind) {
                case just_index_setup_kind::scalar:
                    required = &requirements.scalar_per_sequence;
                    break;
                case just_index_setup_kind::simd:
                    required = &requirements.simd_per_sequence;
                    break;
                case just_index_setup_kind::gpu:
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

/** Return whether this run will really construct a persisted MPS index. */
[[nodiscard]] bool requires_persisted_mps_resource_preflight(
        const e2e_just_index_setup_requirements& requirements) {
    if (requirements.scalar_per_sequence
        || requirements.simd_per_sequence
        || requirements.gpu_per_sequence) {
        return true;
    }
    for (const auto& setup : requirements.matrix_setups) {
        if (setup.minimizer_index == k_minimizers_per_sequence_index) {
            return true;
        }
        if (setup.minimizer_index != k_minimizers_per_fragment_index) {
            throw std::invalid_argument(fmt::format(
                "algorithm-matrix persisted setup resolved unsupported "
                "minimizer_index '{}'",
                setup.minimizer_index));
        }
    }
    return false;
}

[[nodiscard]] e2e_setup_selection e2e_setup_selection_for(
        const e2e_just_index_setup_requirements& requirements) noexcept {
    return e2e_setup_selection{
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
    constexpr std::uintmax_t k_gib = 1024ull * 1024ull * 1024ull;
    if (target_bp < k_per_seq_index_preflight_threshold_bp) return;

    const auto tmp_free = std::filesystem::space("/tmp").available;
    const auto rows = it::query_rows(conn, "SHOW data_directory");
    const std::filesystem::path pg_data_dir =
        (rows.empty() || !rows[0][0].has_value()) ? "/" : rows[0][0].value();
    const auto pg_free = std::filesystem::space(pg_data_dir).available;
    constexpr std::uintmax_t k_required_free = 50ull * k_gib;
    if (tmp_free < k_required_free || pg_free < k_required_free) {
        bioseq_log<logging_level::error>("[bench_e2e] {} requires >=50 GiB free in /tmp and the PG data "
             "directory before minimizers_per_sequence external-sort build "
             "(tmp_free={} GiB, pg_free={} GiB)",
             size_label,
             static_cast<unsigned long long>(tmp_free / k_gib),
             static_cast<unsigned long long>(pg_free / k_gib));
        std::abort();
    }
}

// ---- small JSON helpers shared with the alignment benchmark ----
[[nodiscard]] std::int64_t jint(simdjson::dom::element d, std::string_view k, std::int64_t fb) {
    std::int64_t v = fb; (void)(d[k].get(v) == simdjson::SUCCESS); return v; }
[[nodiscard]] double jdbl(simdjson::dom::element d, std::string_view k, double fb) {
    double v = fb; auto e = d[k]; if (!e.error()) { if (e.value_unsafe().is<double>()) v = e.value_unsafe().get_double().value_unsafe(); else if (e.value_unsafe().is<std::int64_t>()) v = static_cast<double>(e.value_unsafe().get_int64().value_unsafe()); } return v; }
[[nodiscard]] std::string jstr(simdjson::dom::element d, std::string_view k, std::string_view fb) {
    std::string_view v = fb; (void)(d[k].get(v) == simdjson::SUCCESS); return std::string(v); }
[[nodiscard]] bool jbool(simdjson::dom::element d, std::string_view k, bool fb) {
    bool v = fb; (void)(d[k].get(v) == simdjson::SUCCESS); return v; }
[[nodiscard]] std::optional<bool> jbool_optional(
        simdjson::dom::element d,
        std::string_view k) {
    bool value = false;
    return d[k].get(value) == simdjson::SUCCESS
        ? std::optional<bool>{value}
        : std::nullopt;
}

// NO_POSTGRES dna4/dna5 set (the pg `pgseq_supported_alphabets` lives in a
// postgres-only module, so the bench instantiates the same machinery locally).
using bench_supported_alphabets = supported_alphabets<
    alphabet_entry<seq::dna4, "dna4">,
    alphabet_entry<seq::dna5, "dna5">>;

template <class p_handler>
inline void dispatch_bench_alphabet(
    std::string_view name,
    p_handler&& handler) {
    std::string owned{name};
    const bool matched = bench_supported_alphabets::dispatch_alphabet_by_name(
        owned.c_str(),
        [&handler]<class p_entry>() {
            using alphabet_type = typename p_entry::alphabet_type;
            handler.template operator()<alphabet_type>();
        });
    if (!matched) {
        bioseq_log<logging_level::error>("[bench_e2e] unsupported alphabet '{}'", name);
        std::abort();
    }
}

// ---- §10.0c — zero-copy FASTA emission over the IO sequence_writer ----
template <sequence_alphabet p_alphabet>
using bench_writer = bioseq::io::sequence_writer<
    p_alphabet, bioseq::io::sequence_file_format::fasta>;
template <sequence_alphabet p_alphabet>
using bench_view =
    bioseq::io::compact_sequence_view<bench_config<p_alphabet>>;

// One zero-copy view over [unit_off, unit_off+unit_count) of a generated range.
template <sequence_alphabet p_alphabet>
[[nodiscard]] inline bench_view<p_alphabet> view_slice(
        const bench_sequence_range<p_alphabet>& seq,
        std::size_t unit_off, std::size_t unit_count, std::size_t padding) {
    return bench_view<p_alphabet>{
        seq.packed_units.data() + unit_off, unit_count, padding};
}

// ---- §11 — FASTA writers / minimap2 drivers / result parsers ----

/// Write `seq` to `path` as FASTA. record_bp == 0 => one record "<base>0" over the
/// whole range. Otherwise cut into records "<base>0".."<base>N" of `record_bp`
/// bases each via the generic, zero-copy split_units — every record is a
/// compact_sequence_view fed straight to the bounded SIMD IO writer (zero
/// re-pack, RSS-bounded). Bodies are line-wrapped (k_fasta_line_width) so
/// bioseq.load's reader slab stays bounded.
template <sequence_alphabet p_alphabet>
inline void write_fasta(
        const std::string& path,
        const bench_sequence_range<p_alphabet>& seq,
                        std::string_view base_name, std::size_t record_bp) {
    bioseq::io::writer_options wo = bioseq::io::default_writer_options;
    wo.fasta_line_width = k_fasta_line_width;
    auto writer_or = bench_writer<p_alphabet>::open(path, wo);
    if (!writer_or) {
        bioseq_log<logging_level::error>("[bench_e2e] cannot open '{}': {}", path, writer_or.error()); std::abort();
    }
    auto& writer = *writer_or;
    const auto emit = [&writer, &path, &seq, base_name](
                          std::size_t idx,
                          std::size_t off,
                          std::size_t cnt,
                          std::size_t pad) {
        if (!writer.write_record_streamed(fmt::format("{}{}", base_name, idx),
                                          view_slice<p_alphabet>(
                                              seq, off, cnt, pad),
                                          k_fasta_stream_chunk_bp)) {
            bioseq_log<logging_level::error>("[bench_e2e] write_record_streamed failed for '{}'", path); std::abort();
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
        bioseq_log<logging_level::error>("[bench_e2e] close '{}': {}", path, c.error()); std::abort();
    }
}

template <sequence_alphabet p_alphabet>
inline void write_single_record_fasta(const std::string& path, std::string_view name,
                                      const bench_sequence_range<p_alphabet>& seq) {
    write_fasta<p_alphabet>(
        path, seq, name, /*record_bp=*/0);       // one record "<name>0"
}
template <sequence_alphabet p_alphabet>
inline void write_split_fasta(const std::string& path, std::string_view base,
                              const bench_sequence_range<p_alphabet>& seq,
                              std::size_t split_bp) {
    write_fasta<p_alphabet>(path, seq, base, split_bp);
}

/** @brief CRC-64 over the exact decoded target bytes persisted by Bioseq. */
template <sequence_alphabet p_alphabet>
[[nodiscard]] std::uint64_t decoded_target_content_crc64(
        const bench_sequence_range<p_alphabet>& sequence) {
    using traits = typename bench_sequence_range<p_alphabet>::traits;
    using storage_unit = typename traits::storage_unit;
    constexpr std::size_t k_letters_per_unit = traits::letters_per_unit;
    constexpr std::size_t k_bits_per_character = traits::bits_per_char;
    const std::size_t units_per_slice = std::max<std::size_t>(
        std::size_t{1}, k_fasta_stream_chunk_bp / k_letters_per_unit);
    std::vector<std::uint8_t> decoded;
    decoded.reserve(k_fasta_stream_chunk_bp);
    bioseq::indexing::crc64_ecma182_accumulator checksum;
    for (std::size_t unit_offset = 0; unit_offset < sequence.size();
         unit_offset += units_per_slice) {
        const std::size_t unit_count = std::min(
            units_per_slice, sequence.size() - unit_offset);
        const bool final_slice = unit_offset + unit_count == sequence.size();
        const std::size_t letters = unit_count * k_letters_per_unit
            - (final_slice ? sequence.padding() : 0u);
        decoded.resize(letters);
        bioseq::simd::active_target::decode_words_to_ascii<
            p_alphabet,
            storage_unit,
            k_bits_per_character,
            k_letters_per_unit>(
                sequence.packed_units.data() + unit_offset,
                decoded.data(),
                letters);
        checksum.update(std::as_bytes(std::span{
            reinterpret_cast<const char*>(decoded.data()),
            decoded.size()}));
    }
    return checksum.checksum();
}

template <sequence_alphabet p_alphabet>
[[nodiscard]] std::string decode_target_window(
        const bench_sequence_range<p_alphabet>& target,
        const std::size_t begin_bp,
        const std::size_t end_bp) {
    using traits = typename bench_sequence_range<p_alphabet>::traits;
    if (begin_bp > end_bp || end_bp > target.bp_size()) return {};
    auto iterator = target.template iter<alphabet_symbol>(
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

template <sequence_alphabet p_alphabet>
void write_multi_query_fasta(
        const std::string& path,
        std::span<const e2e_query_truth> queries)
{
    (void)sizeof(p_alphabet);
    std::ofstream out(path);
    if (!out) {
        throw std::runtime_error("failed to open multi-query FASTA: " + path);
    }
    for (const e2e_query_truth& query : queries) {
        out << ">query" << query.id << "\n";
        for (std::size_t pos = 0; pos < query.bases.size(); pos += k_fasta_line_width) {
            out << std::string_view{
                query.bases.data() + pos,
                std::min(k_fasta_line_width, query.bases.size() - pos)} << "\n";
        }
    }
}

enum class minimap2_receipt_mode : std::uint8_t {
    index_setup,
    no_index,
    prebuilt_index,
};

[[nodiscard]] constexpr std::string_view minimap2_receipt_mode_name(
        const minimap2_receipt_mode mode) noexcept {
    switch (mode) {
    case minimap2_receipt_mode::index_setup:
        return "index_setup";
    case minimap2_receipt_mode::no_index:
        return "no_index";
    case minimap2_receipt_mode::prebuilt_index:
        return "prebuilt_index";
    }
    return "invalid";
}

struct minimap2_private_workspace final {
    std::filesystem::path root{};
    std::filesystem::path stdout_path{};
    std::filesystem::path stderr_path{};
    std::filesystem::path receipt_path{};
    int stdout_fd = -1;
    int stderr_fd = -1;
    int receipt_fd = -1;

    minimap2_private_workspace() = default;
    minimap2_private_workspace(const minimap2_private_workspace&) = delete;
    minimap2_private_workspace& operator=(const minimap2_private_workspace&) = delete;
    minimap2_private_workspace(minimap2_private_workspace&& other) noexcept
        : root(std::move(other.root)),
          stdout_path(std::move(other.stdout_path)),
          stderr_path(std::move(other.stderr_path)),
          receipt_path(std::move(other.receipt_path)),
          stdout_fd(std::exchange(other.stdout_fd, -1)),
          stderr_fd(std::exchange(other.stderr_fd, -1)),
          receipt_fd(std::exchange(other.receipt_fd, -1)) {}

    ~minimap2_private_workspace() noexcept {
        if (stdout_fd >= 0) (void)::close(stdout_fd);
        if (stderr_fd >= 0) (void)::close(stderr_fd);
        if (receipt_fd >= 0) (void)::close(receipt_fd);
        if (!root.empty()) {
            std::error_code error;
            (void)std::filesystem::remove_all(root, error);
        }
    }
};

[[nodiscard]] std::string unpredictable_minimap2_run_token() {
    std::array<unsigned char, 32> random_bytes{};
    if (::getentropy(random_bytes.data(), random_bytes.size()) != 0) {
        throw std::runtime_error(
            "getentropy failed while creating minimap2 receipt token");
    }
    constexpr char hexadecimal[] = "0123456789abcdef";
    std::string token = "e2e-";
    token.reserve(4u + random_bytes.size() * 2u);
    for (const unsigned char byte : random_bytes) {
        token.push_back(hexadecimal[byte >> 4u]);
        token.push_back(hexadecimal[byte & 0x0fu]);
    }
    return token;
}

/** Read the parent side of the helper-declared CLOCK_MONOTONIC domain. */
[[nodiscard]] std::uint64_t minimap2_child_lifetime_clock_now_ns() {
    timespec value{};
    if (::clock_gettime(CLOCK_MONOTONIC, &value) != 0 ||
        value.tv_sec < 0 || value.tv_nsec < 0 ||
        value.tv_nsec >= 1'000'000'000L) {
        throw std::runtime_error(
            "CLOCK_MONOTONIC failed around minimap2 receipt child");
    }
    constexpr std::uint64_t billion = 1'000'000'000u;
    const std::uint64_t seconds = static_cast<std::uint64_t>(value.tv_sec);
    const std::uint64_t nanoseconds =
        static_cast<std::uint64_t>(value.tv_nsec);
    if (seconds >
        ((std::numeric_limits<std::uint64_t>::max)() - nanoseconds) /
            billion) {
        throw std::runtime_error(
            "CLOCK_MONOTONIC overflowed around minimap2 receipt child");
    }
    return seconds * billion + nanoseconds;
}

[[nodiscard]] int open_minimap2_private_file(
        const std::filesystem::path& path) {
    const int descriptor = ::open(
        path.c_str(), O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC | O_NOFOLLOW,
        S_IRUSR | S_IWUSR);
    if (descriptor < 0) {
        throw std::runtime_error(
            "cannot create private minimap2 child file: " + path.string());
    }
    struct stat status{};
    if (::fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode) ||
        status.st_uid != ::geteuid() || status.st_nlink != 1 ||
        (status.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
        (void)::close(descriptor);
        (void)::unlink(path.c_str());
        throw std::runtime_error(
            "minimap2 child file failed private-regular-file validation");
    }
    return descriptor;
}

[[nodiscard]] minimap2_private_workspace make_minimap2_private_workspace(
        const std::string& token) {
    minimap2_private_workspace workspace;
    const std::filesystem::path temporary_root =
        std::filesystem::temp_directory_path();
    std::string directory_template =
        (temporary_root / ("bioseq-e2e-minimap2-" + token.substr(4u, 16u) +
                           ".XXXXXX")).string();
    if (::mkdtemp(directory_template.data()) == nullptr) {
        throw std::runtime_error(
            "cannot create private minimap2 parent workspace");
    }
    workspace.root = directory_template;
    struct stat root_status{};
    if (::lstat(workspace.root.c_str(), &root_status) != 0 ||
        !S_ISDIR(root_status.st_mode) || root_status.st_uid != ::geteuid() ||
        (root_status.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
        std::error_code error;
        (void)std::filesystem::remove_all(workspace.root, error);
        throw std::runtime_error(
            "minimap2 parent workspace failed owner-only validation");
    }
    workspace.stdout_path = workspace.root / "stdout.paf";
    workspace.stderr_path = workspace.root / "stderr.log";
    workspace.receipt_path = workspace.root / "receipt.json";
    try {
        workspace.stdout_fd = open_minimap2_private_file(workspace.stdout_path);
        workspace.stderr_fd = open_minimap2_private_file(workspace.stderr_path);
        workspace.receipt_fd = open_minimap2_private_file(workspace.receipt_path);
    } catch (...) {
        if (workspace.stdout_fd >= 0) {
            (void)::close(std::exchange(workspace.stdout_fd, -1));
        }
        if (workspace.stderr_fd >= 0) {
            (void)::close(std::exchange(workspace.stderr_fd, -1));
        }
        if (workspace.receipt_fd >= 0) {
            (void)::close(std::exchange(workspace.receipt_fd, -1));
        }
        std::error_code error;
        (void)std::filesystem::remove_all(workspace.root, error);
        workspace.root.clear();
        throw;
    }
    return workspace;
}

[[nodiscard]] std::string read_minimap2_private_file(
        const int descriptor,
        const std::size_t maximum_bytes,
        const std::string_view label) {
    struct stat status{};
    if (::fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode) ||
        status.st_uid != ::geteuid() || status.st_nlink != 1 ||
        (status.st_mode & (S_IRWXG | S_IRWXO)) != 0 || status.st_size < 0 ||
        static_cast<std::uintmax_t>(status.st_size) > maximum_bytes) {
        throw std::runtime_error(
            fmt::format("minimap2 {} is not a bounded regular file", label));
    }
    const std::size_t size = static_cast<std::size_t>(status.st_size);
    if (::lseek(descriptor, 0, SEEK_SET) != 0) {
        throw std::runtime_error(
            fmt::format("cannot rewind minimap2 {}", label));
    }
    std::string result(size, '\0');
    std::size_t offset = 0;
    while (offset < result.size()) {
        const ssize_t count =
            ::read(descriptor, result.data() + offset, result.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            throw std::runtime_error(
                fmt::format("minimap2 {} is partial", label));
        }
        offset += static_cast<std::size_t>(count);
    }
    char trailing = '\0';
    ssize_t trailing_count = -1;
    do {
        trailing_count = ::read(descriptor, &trailing, 1);
    } while (trailing_count < 0 && errno == EINTR);
    if (trailing_count != 0) {
        throw std::runtime_error(
            fmt::format("minimap2 {} changed while being read", label));
    }
    return result;
}

struct minimap2_phase_span {
    std::uint64_t begin_ns = 0;
    std::uint64_t end_ns = 0;
    std::uint32_t phase = 0;
    std::uint32_t thread_ordinal = 0;
};

struct minimap2_union_result {
    std::uint64_t duration_ns = 0;
    std::uint64_t merged_interval_count = 0;
    bool valid = true;
};

[[nodiscard]] minimap2_union_result recompute_minimap2_union(
        const std::vector<minimap2_phase_span>& spans,
        const std::uint32_t selected_phase) noexcept {
    // The sole caller validates canonical raw-span order before any union.
    // Filtering preserves that order, so no allocation or second sort is needed.
    std::uint64_t duration_ns = 0u;
    std::uint64_t merged_interval_count = 0u;
    std::uint64_t merged_begin = 0u;
    std::uint64_t merged_end = 0u;
    bool has_merged_span = false;
    for (std::size_t index = 0; index < spans.size(); ++index) {
        const minimap2_phase_span& span = spans[index];
        if (span.end_ns < span.begin_ns ||
            (index != 0u && span.begin_ns < spans[index - 1u].begin_ns)) {
            return {.valid = false};
        }
        if (selected_phase != 0u && span.phase != selected_phase) continue;
        if (!has_merged_span) {
            merged_begin = span.begin_ns;
            merged_end = span.end_ns;
            has_merged_span = true;
            continue;
        }
        if (span.begin_ns <= merged_end) {
            merged_end = (std::max)(merged_end, span.end_ns);
            continue;
        }
        const std::uint64_t duration = merged_end - merged_begin;
        if (duration >
                (std::numeric_limits<std::uint64_t>::max)() - duration_ns ||
            merged_interval_count ==
                (std::numeric_limits<std::uint64_t>::max)()) {
            return {.valid = false};
        }
        duration_ns += duration;
        ++merged_interval_count;
        merged_begin = span.begin_ns;
        merged_end = span.end_ns;
    }
    if (has_merged_span) {
        const std::uint64_t duration = merged_end - merged_begin;
        if (duration >
                (std::numeric_limits<std::uint64_t>::max)() - duration_ns ||
            merged_interval_count ==
                (std::numeric_limits<std::uint64_t>::max)()) {
            return {.valid = false};
        }
        duration_ns += duration;
        ++merged_interval_count;
    }
    return {
        .duration_ns = duration_ns,
        .merged_interval_count = merged_interval_count,
        .valid = true,
    };
}

struct minimap2_fnv1a64 {
    std::uint64_t value = 14'695'981'039'346'656'037ULL;

    void append_byte(const std::uint8_t byte) noexcept {
        value ^= byte;
        value *= 1'099'511'628'211ULL;
    }
    void append_u32_le(const std::uint32_t number) noexcept {
        for (unsigned int shift = 0; shift < 32u; shift += 8u) {
            append_byte(static_cast<std::uint8_t>(number >> shift));
        }
    }
    void append_u64_le(const std::uint64_t number) noexcept {
        for (unsigned int shift = 0; shift < 64u; shift += 8u) {
            append_byte(static_cast<std::uint8_t>(number >> shift));
        }
    }
    void append_string(const std::string_view value_to_append) noexcept {
        append_u64_le(static_cast<std::uint64_t>(value_to_append.size()));
        for (const unsigned char byte : value_to_append) append_byte(byte);
    }
};

[[nodiscard]] std::string minimap2_hex_u64(const std::uint64_t value) {
    constexpr char hexadecimal[] = "0123456789abcdef";
    std::string result(16u, '0');
    for (std::size_t index = 0; index < result.size(); ++index) {
        const unsigned int shift =
            static_cast<unsigned int>((result.size() - index - 1u) * 4u);
        result[index] = hexadecimal[(value >> shift) & 0x0fu];
    }
    return result;
}

[[nodiscard]] bool minimap2_json_exact_keys(
        const nlohmann::json& object,
        const std::initializer_list<std::string_view> expected) {
    if (!object.is_object() || object.size() != expected.size()) return false;
    return std::ranges::all_of(expected, [&object](const std::string_view key) {
        return object.contains(std::string{key});
    });
}

[[nodiscard]] std::uint64_t minimap2_json_u64(
        const nlohmann::json& object,
        const std::string_view key) {
    const auto found = object.find(std::string{key});
    if (found == object.end()) {
        throw std::runtime_error(
            fmt::format("minimap2 receipt is missing {}", key));
    }
    if (found->is_number_unsigned()) return found->get<std::uint64_t>();
    if (found->is_number_integer()) {
        const std::int64_t value = found->get<std::int64_t>();
        if (value >= 0) return static_cast<std::uint64_t>(value);
    }
    throw std::runtime_error(
        fmt::format("minimap2 receipt {} is not a u64", key));
}

struct validated_minimap2_receipt {
    std::uint64_t child_pid = 0;
    std::uint64_t cigar_interval_count = 0;
    std::uint64_t mapq_interval_count = 0;
    std::uint64_t cigar_union_ns = 0;
    std::uint64_t mapq_union_ns = 0;
    std::uint64_t combined_union_ns = 0;
    std::uint64_t observed_index_parts = 0;
    std::string checksum{};
    nlohmann::json summary = nullptr;
};

[[nodiscard]] validated_minimap2_receipt validate_minimap2_receipt(
        const std::string& body,
        const std::string_view expected_token,
        const minimap2_receipt_mode expected_mode,
        const pid_t expected_pid,
        const std::uint64_t expected_queries,
        const std::uint64_t expected_index_parts,
        const std::uint64_t parent_child_lifetime_begin_ns,
        const std::uint64_t parent_child_lifetime_end_ns,
        const std::filesystem::path& parent_cleanup_root) {
    if (body.size() < 3u || body.front() != '{' || body.back() != '\n' ||
        body[body.size() - 2u] != '}' ||
        std::count(body.begin(), body.end(), '\n') != 1) {
        throw std::runtime_error(
            "minimap2 receipt is missing, duplicate, partial, or has trailing data");
    }
    const nlohmann::json receipt = nlohmann::json::parse(
        body.begin(), body.end() - 1, nullptr, false);
    if (receipt.is_discarded() ||
        !minimap2_json_exact_keys(receipt, {
            "schema", "version", "run_token", "mode", "pid", "clock",
            "clock_unit", "interval_semantics", "buffer_capacity", "counts",
            "unions_ns", "merged_interval_counts", "validity", "artifacts",
            "checksum", "raw_intervals"})) {
        throw std::runtime_error("minimap2 receipt has the wrong exact root schema");
    }
    const std::string expected_mode_name{minimap2_receipt_mode_name(expected_mode)};
    if (!receipt["schema"].is_string() ||
        receipt["schema"] != "bioseq.minimap2.e2e-phase-receipt" ||
        minimap2_json_u64(receipt, "version") != 2u ||
        !receipt["run_token"].is_string() ||
        receipt["run_token"].get_ref<const std::string&>() != expected_token ||
        !receipt["mode"].is_string() || receipt["mode"] != expected_mode_name ||
        expected_pid <= 0 ||
        minimap2_json_u64(receipt, "pid") !=
            static_cast<std::uint64_t>(expected_pid) ||
        !receipt["clock"].is_string() || receipt["clock"] != "CLOCK_MONOTONIC" ||
        !receipt["clock_unit"].is_string() || receipt["clock_unit"] != "ns" ||
        !receipt["interval_semantics"].is_string() ||
        receipt["interval_semantics"] != "half_open" ||
        minimap2_json_u64(receipt, "buffer_capacity") != 262'144u) {
        throw std::runtime_error("minimap2 receipt identity does not match its child");
    }

    const nlohmann::json& counts = receipt["counts"];
    if (!minimap2_json_exact_keys(counts, {
            "begin_calls", "end_calls", "cigar_begin_calls",
            "mapq_begin_calls", "claimed_slots", "recorded_intervals",
            "dropped_intervals", "cigar_intervals", "mapq_intervals",
            "threads_observed", "expected_queries", "observed_queries",
            "expected_index_parts", "observed_index_parts"})) {
        throw std::runtime_error("minimap2 receipt counts have the wrong exact schema");
    }
    const std::array<std::uint64_t, 14> count_values{
        minimap2_json_u64(counts, "begin_calls"),
        minimap2_json_u64(counts, "end_calls"),
        minimap2_json_u64(counts, "cigar_begin_calls"),
        minimap2_json_u64(counts, "mapq_begin_calls"),
        minimap2_json_u64(counts, "claimed_slots"),
        minimap2_json_u64(counts, "recorded_intervals"),
        minimap2_json_u64(counts, "dropped_intervals"),
        minimap2_json_u64(counts, "cigar_intervals"),
        minimap2_json_u64(counts, "mapq_intervals"),
        minimap2_json_u64(counts, "threads_observed"),
        minimap2_json_u64(counts, "expected_queries"),
        minimap2_json_u64(counts, "observed_queries"),
        minimap2_json_u64(counts, "expected_index_parts"),
        minimap2_json_u64(counts, "observed_index_parts"),
    };
    const auto [begin_calls, end_calls, cigar_begin_calls, mapq_begin_calls,
                claimed_slots, recorded_intervals, dropped_intervals,
                cigar_intervals, mapq_intervals, threads_observed,
                receipt_expected_queries, observed_queries,
                receipt_expected_index_parts, observed_index_parts] = count_values;
    if (receipt_expected_queries != expected_queries ||
        receipt_expected_index_parts != expected_index_parts ||
        observed_index_parts != expected_index_parts || dropped_intervals != 0u ||
        begin_calls != end_calls || begin_calls != claimed_slots ||
        begin_calls != recorded_intervals || cigar_begin_calls != cigar_intervals ||
        mapq_begin_calls != mapq_intervals ||
        cigar_intervals > recorded_intervals ||
        mapq_intervals != recorded_intervals - cigar_intervals) {
        throw std::runtime_error("minimap2 receipt count equations are inconsistent");
    }

    std::uint64_t expected_cigar_intervals = 0;
    std::uint64_t expected_mapq_intervals = 0;
    std::uint64_t expected_total_intervals = 0;
    bool expected_count_arithmetic_valid = true;
    if (expected_mode != minimap2_receipt_mode::index_setup) {
        if (expected_queries == 0u || expected_index_parts == 0u ||
            expected_index_parts ==
            (std::numeric_limits<std::uint64_t>::max)()) {
            expected_count_arithmetic_valid = false;
        }
        const std::uint64_t expected_mapq_parts =
            expected_index_parts > 1u && expected_count_arithmetic_valid
                ? expected_index_parts + 1u
                                      : expected_index_parts;
        expected_count_arithmetic_valid = expected_count_arithmetic_valid &&
            expected_index_parts <=
                (std::numeric_limits<std::uint64_t>::max)() / expected_queries &&
            expected_mapq_parts <=
                (std::numeric_limits<std::uint64_t>::max)() / expected_queries;
        if (expected_count_arithmetic_valid) {
            expected_cigar_intervals = expected_queries * expected_index_parts;
            expected_mapq_intervals = expected_queries * expected_mapq_parts;
            expected_count_arithmetic_valid = expected_mapq_intervals <=
                (std::numeric_limits<std::uint64_t>::max)() -
                    expected_cigar_intervals;
        }
        if (expected_count_arithmetic_valid) {
            expected_total_intervals =
                expected_cigar_intervals + expected_mapq_intervals;
            expected_count_arithmetic_valid =
                expected_total_intervals <= 262'144u;
        }
    }
    const bool setup_counts_valid =
        expected_mode == minimap2_receipt_mode::index_setup &&
        expected_queries == 0u && observed_queries == 0u && begin_calls == 0u &&
        cigar_intervals == 0u && mapq_intervals == 0u && threads_observed == 0u;
    const bool mapping_counts_valid =
        expected_mode != minimap2_receipt_mode::index_setup &&
        expected_queries != 0u && observed_queries == expected_queries &&
        expected_count_arithmetic_valid &&
        cigar_intervals == expected_cigar_intervals &&
        mapq_intervals == expected_mapq_intervals &&
        recorded_intervals == expected_total_intervals;
    if (!setup_counts_valid && !mapping_counts_valid) {
        throw std::runtime_error(
            "minimap2 receipt workload counts do not match the requested run");
    }

    if (!receipt["raw_intervals"].is_array() ||
        receipt["raw_intervals"].size() != recorded_intervals) {
        throw std::runtime_error("minimap2 receipt raw span count is inconsistent");
    }
    std::vector<minimap2_phase_span> spans;
    spans.reserve(receipt["raw_intervals"].size());
    std::uint64_t counted_cigar = 0;
    std::uint64_t counted_mapq = 0;
    std::optional<std::tuple<std::uint64_t, std::uint64_t,
                             std::uint32_t, std::uint32_t>> previous;
    for (const nlohmann::json& raw : receipt["raw_intervals"]) {
        if (!minimap2_json_exact_keys(
                raw, {"phase", "begin_ns", "end_ns", "thread_ordinal"}) ||
            !raw["phase"].is_string()) {
            throw std::runtime_error("minimap2 raw span has the wrong exact schema");
        }
        const std::string phase_name = raw["phase"].get<std::string>();
        const std::uint32_t phase = phase_name == "cigar" ? 1u
            : phase_name == "mapq" ? 2u : 0u;
        const std::uint64_t begin_ns = minimap2_json_u64(raw, "begin_ns");
        const std::uint64_t end_ns = minimap2_json_u64(raw, "end_ns");
        const std::uint64_t raw_thread =
            minimap2_json_u64(raw, "thread_ordinal");
        if (phase == 0u || raw_thread >
                (std::numeric_limits<std::uint32_t>::max)() ||
            end_ns < begin_ns
            || begin_ns < parent_child_lifetime_begin_ns
            || end_ns > parent_child_lifetime_end_ns
            || raw_thread >= threads_observed) {
            throw std::runtime_error(
                "minimap2 raw span is invalid or outside the parent-observed "
                "child lifetime envelope");
        }
        const auto identity = std::tuple{
            begin_ns, end_ns, phase, static_cast<std::uint32_t>(raw_thread)};
        if (previous.has_value() && identity < *previous) {
            throw std::runtime_error("minimap2 raw spans are not canonical-sorted");
        }
        previous = identity;
        spans.push_back({
            .begin_ns = begin_ns,
            .end_ns = end_ns,
            .phase = phase,
            .thread_ordinal = static_cast<std::uint32_t>(raw_thread),
        });
        if (phase == 1u) ++counted_cigar;
        else ++counted_mapq;
    }
    if (counted_cigar != cigar_intervals || counted_mapq != mapq_intervals) {
        throw std::runtime_error("minimap2 raw span phase counts are inconsistent");
    }

    const minimap2_union_result cigar_union =
        recompute_minimap2_union(spans, 1u);
    const minimap2_union_result mapq_union =
        recompute_minimap2_union(spans, 2u);
    const minimap2_union_result combined_union =
        recompute_minimap2_union(spans, 0u);
    const nlohmann::json& unions = receipt["unions_ns"];
    const nlohmann::json& merged = receipt["merged_interval_counts"];
    if (!cigar_union.valid || !mapq_union.valid || !combined_union.valid ||
        !minimap2_json_exact_keys(unions, {"cigar", "mapq", "cigar_or_mapq"}) ||
        !minimap2_json_exact_keys(merged, {"cigar", "mapq", "cigar_or_mapq"}) ||
        minimap2_json_u64(unions, "cigar") != cigar_union.duration_ns ||
        minimap2_json_u64(unions, "mapq") != mapq_union.duration_ns ||
        minimap2_json_u64(unions, "cigar_or_mapq") != combined_union.duration_ns ||
        minimap2_json_u64(merged, "cigar") != cigar_union.merged_interval_count ||
        minimap2_json_u64(merged, "mapq") != mapq_union.merged_interval_count ||
        minimap2_json_u64(merged, "cigar_or_mapq") !=
            combined_union.merged_interval_count ||
        combined_union.duration_ns
            > parent_child_lifetime_end_ns
                - parent_child_lifetime_begin_ns) {
        throw std::runtime_error(fmt::format(
            "minimap2 receipt unions do not recompute exactly: mode={} "
            "valid=[{},{},{}] reported_unions_ns={} "
            "recomputed_unions_ns=[{},{},{}] reported_merged={} "
            "recomputed_merged=[{},{},{}] child_lifetime_ns=[{},{}] "
            "raw_interval_count={}",
            expected_mode_name,
            cigar_union.valid, mapq_union.valid, combined_union.valid,
            unions.dump().substr(0u, 512u),
            cigar_union.duration_ns, mapq_union.duration_ns,
            combined_union.duration_ns,
            merged.dump().substr(0u, 512u),
            cigar_union.merged_interval_count, mapq_union.merged_interval_count,
            combined_union.merged_interval_count,
            parent_child_lifetime_begin_ns, parent_child_lifetime_end_ns,
            spans.size()));
    }

    const nlohmann::json& validity = receipt["validity"];
    if (!minimap2_json_exact_keys(validity, {
            "complete", "cleanup_complete", "split_io_valid", "clock_valid",
            "buffer_valid", "buffer_overflow", "intervals_valid", "arithmetic_valid",
            "capacity_contract_valid", "query_count_valid", "index_part_count_valid",
            "phase_count_contract_valid", "workload_valid", "late_event", "valid"})) {
        throw std::runtime_error("minimap2 receipt validity has the wrong exact schema");
    }
    for (const std::string_view key : {
            "complete", "cleanup_complete", "split_io_valid", "clock_valid", "buffer_valid",
            "intervals_valid", "arithmetic_valid", "capacity_contract_valid",
            "query_count_valid",
            "index_part_count_valid", "phase_count_contract_valid",
            "workload_valid", "valid"}) {
        if (!validity[std::string{key}].is_boolean() ||
            !validity[std::string{key}].get<bool>()) {
            throw std::runtime_error(
                fmt::format("minimap2 receipt validity {} is false", key));
        }
    }
    if (!validity["buffer_overflow"].is_boolean() ||
        validity["buffer_overflow"].get<bool>() ||
        !validity["late_event"].is_boolean() || validity["late_event"].get<bool>()) {
        throw std::runtime_error("minimap2 receipt reports overflow or a late event");
    }

    const nlohmann::json& artifacts = receipt["artifacts"];
    if (!minimap2_json_exact_keys(
            artifacts, {"parent_cleanup_root", "split_temp_directory", "split_prefix"}) ||
        !artifacts["parent_cleanup_root"].is_string() ||
        !artifacts["split_temp_directory"].is_string() ||
        !artifacts["split_prefix"].is_string() ||
        artifacts["parent_cleanup_root"].get<std::string>() !=
            parent_cleanup_root.string()) {
        throw std::runtime_error("minimap2 receipt cleanup ownership is invalid");
    }
    const std::string split_directory =
        artifacts["split_temp_directory"].get<std::string>();
    const std::string split_prefix = artifacts["split_prefix"].get<std::string>();
    const std::string owned_prefix = parent_cleanup_root.string() + "/";
    if ((!split_directory.empty() &&
         (!std::string_view{split_directory}.starts_with(owned_prefix) ||
          std::filesystem::exists(split_directory))) ||
        (!split_prefix.empty() &&
         !std::string_view{split_prefix}.starts_with(owned_prefix))) {
        throw std::runtime_error("minimap2 split workspace escaped parent ownership");
    }

    const nlohmann::json& checksum = receipt["checksum"];
    if (!minimap2_json_exact_keys(checksum, {
            "algorithm", "canonicalization", "offset_basis_hex", "prime_hex",
            "byte_order", "string_encoding", "field_order", "count_order",
            "raw_record_order", "raw_sort_order", "phase_codes", "value_hex"}) ||
        !checksum["algorithm"].is_string() ||
        checksum["algorithm"] != "fnv-1a-64" ||
        !checksum["canonicalization"].is_string() ||
        checksum["canonicalization"] !=
            "bioseq.minimap2.e2e-phase-receipt.checksum.v1" ||
        !checksum["offset_basis_hex"].is_string() ||
        checksum["offset_basis_hex"] != "cbf29ce484222325" ||
        !checksum["prime_hex"].is_string() ||
        checksum["prime_hex"] != "00000100000001b3" ||
        !checksum["byte_order"].is_string() ||
        checksum["byte_order"] != "little_endian" ||
        !checksum["string_encoding"].is_string() ||
        checksum["string_encoding"] !=
            "u64le_byte_length_then_exact_bytes" ||
        checksum["field_order"] != nlohmann::json::array({
            "canonicalization_domain:string", "schema:string",
            "schema_version:u64", "run_token:string", "mode:string",
            "pid:u64", "counts:u64[]", "raw_records[]"}) ||
        checksum["count_order"] != nlohmann::json::array({
            "begin_calls", "end_calls", "cigar_begin_calls",
            "mapq_begin_calls", "claimed_slots", "recorded_intervals",
            "dropped_intervals", "cigar_intervals", "mapq_intervals",
            "threads_observed", "expected_queries", "observed_queries",
            "expected_index_parts", "observed_index_parts"}) ||
        checksum["raw_record_order"] != nlohmann::json::array({
            "phase_code:u32", "begin_ns:u64", "end_ns:u64",
            "thread_ordinal:u32"}) ||
        checksum["raw_sort_order"] != nlohmann::json::array({
            "begin_ns", "end_ns", "phase_code", "thread_ordinal"}) ||
        !minimap2_json_exact_keys(checksum["phase_codes"], {"cigar", "mapq"}) ||
        minimap2_json_u64(checksum["phase_codes"], "cigar") != 1u ||
        minimap2_json_u64(checksum["phase_codes"], "mapq") != 2u ||
        !checksum["value_hex"].is_string()) {
        throw std::runtime_error("minimap2 receipt checksum schema is invalid");
    }
    minimap2_fnv1a64 canonical_checksum;
    canonical_checksum.append_string(
        "bioseq.minimap2.e2e-phase-receipt.checksum.v1");
    canonical_checksum.append_string("bioseq.minimap2.e2e-phase-receipt");
    canonical_checksum.append_u64_le(2u);
    canonical_checksum.append_string(expected_token);
    canonical_checksum.append_string(expected_mode_name);
    canonical_checksum.append_u64_le(static_cast<std::uint64_t>(expected_pid));
    for (const std::uint64_t count : count_values) {
        canonical_checksum.append_u64_le(count);
    }
    for (const minimap2_phase_span& span : spans) {
        canonical_checksum.append_u32_le(span.phase);
        canonical_checksum.append_u64_le(span.begin_ns);
        canonical_checksum.append_u64_le(span.end_ns);
        canonical_checksum.append_u32_le(span.thread_ordinal);
    }
    const std::string checksum_hex =
        checksum["value_hex"].get<std::string>();
    if (checksum_hex != minimap2_hex_u64(canonical_checksum.value)) {
        throw std::runtime_error("minimap2 receipt checksum does not recompute");
    }

    return {
        .child_pid = static_cast<std::uint64_t>(expected_pid),
        .cigar_interval_count = cigar_intervals,
        .mapq_interval_count = mapq_intervals,
        .cigar_union_ns = cigar_union.duration_ns,
        .mapq_union_ns = mapq_union.duration_ns,
        .combined_union_ns = combined_union.duration_ns,
        .observed_index_parts = observed_index_parts,
        .checksum = checksum_hex,
        .summary = nlohmann::json{
            {"schema", "bioseq.minimap2.e2e-phase-receipt/validated-summary"},
            {"version", 2},
            {"receipt_schema_version", 2},
            {"mode", expected_mode_name},
            {"pid", expected_pid},
            {"checksum", checksum_hex},
            {"checksum_contract", checksum},
            {"counts", counts},
            {"unions_ns", unions},
            {"merged_interval_counts", merged},
            {"validity", validity},
            {"artifacts", artifacts},
            {"raw_interval_count", spans.size()},
            {"child_lifetime_envelope_contained", true},
            {"framework_repeat_association", "program_order"},
            {"framework_outer_timestamp_containment_claimed", false},
        },
    };
}

[[nodiscard]] std::filesystem::path preserve_minimap2_receipt_artifact(
        const std::string_view token,
        const std::string_view receipt) {
    const std::filesystem::path directory =
        std::filesystem::path{BIOSEQ_E2E_REPORT_DIR} / "minimap2_phase_receipts";
    std::filesystem::create_directories(directory);
    const std::filesystem::path path = directory / (std::string{token} + ".json");
    const int descriptor = ::open(
        path.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW,
        S_IRUSR | S_IWUSR);
    if (descriptor < 0) {
        throw std::runtime_error("cannot create minimap2 receipt artifact");
    }
    std::size_t written = 0;
    while (written < receipt.size()) {
        const ssize_t count = ::write(
            descriptor, receipt.data() + written, receipt.size() - written);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            (void)::close(descriptor);
            (void)::unlink(path.c_str());
            throw std::runtime_error("cannot write minimap2 receipt artifact");
        }
        written += static_cast<std::size_t>(count);
    }
    const int sync_status = ::fsync(descriptor);
    const int close_status = ::close(descriptor);
    if (sync_status != 0 || close_status != 0) {
        (void)::unlink(path.c_str());
        throw std::runtime_error("cannot finalize minimap2 receipt artifact");
    }
    return path;
}

struct minimap2_helper_result {
    std::string stdout_text{};
    validated_minimap2_receipt receipt{};
    std::string artifact_path{};
    std::string artifact_digest{};
};

struct logical_minimap2_chunk_result {
    std::size_t query_begin = 0;
    std::size_t query_end = 0;
    std::uint64_t measured_ns = 0;
    minimap2_helper_result helper{};
};

void kill_and_reap_minimap2_child(const pid_t child_pid) noexcept {
    if (child_pid <= 0) return;
    (void)::kill(-child_pid, SIGKILL);
    (void)::kill(child_pid, SIGKILL);
    int status = 0;
    pid_t waited = -1;
    do {
        waited = ::waitpid(child_pid, &status, 0);
    } while (waited < 0 && errno == EINTR);
}

struct minimap2_child_wait_result {
    int status = 0;
    bool timed_out = false;
};

/**
 * Direct-helper state prepared outside the framework measurement window.
 *
 * Only owning strings are retained here.  The argv pointer vector and the
 * POSIX spawn actions/attributes are constructed in the measured function,
 * after this move-only value has reached its final address.
 */
struct prepared_minimap2_helper_invocation {
    minimap2_private_workspace workspace{};
    std::string token{};
    minimap2_receipt_mode mode = minimap2_receipt_mode::prebuilt_index;
    std::uint64_t expected_queries = 0;
    std::uint64_t expected_index_parts = 0;
    std::vector<std::string> arguments{};
    bool preserve_receipt = false;
    std::chrono::milliseconds timeout{};
};

/** Spawn/wait evidence whose file parsing and receipt validation are deferred. */
struct pending_minimap2_helper_result {
    minimap2_private_workspace workspace{};
    std::string token{};
    minimap2_receipt_mode mode = minimap2_receipt_mode::prebuilt_index;
    std::uint64_t expected_queries = 0;
    std::uint64_t expected_index_parts = 0;
    pid_t child_pid = -1;
    std::uint64_t parent_child_lifetime_begin_ns = 0;
    std::uint64_t parent_child_lifetime_end_ns = 0;
    minimap2_child_wait_result child_wait{};
    bool preserve_receipt = false;
};

/** Signal only the spawn-owned process group and its exact leader PID. */
void signal_minimap2_child_ownership(
        const pid_t child_pid,
        const int signal_number) noexcept {
    if (child_pid <= 0) return;
    (void)::kill(-child_pid, signal_number);
    (void)::kill(child_pid, signal_number);
}

/**
 * Observe one exact child without polling. waitid leaves the exited child
 * waitable, preventing PID reuse until the watchdog is cancelled and the
 * subsequent exact waitpid reaps it.
 */
[[nodiscard]] minimap2_child_wait_result wait_for_minimap2_child(
        const pid_t child_pid,
        const std::chrono::milliseconds timeout) {
    struct watchdog_state {
        std::mutex mutex{};
        std::condition_variable changed{};
        bool exit_observed = false;
        bool cancelled = false;
        bool timed_out = false;
    } state;
    const auto finished = [&state]() noexcept {
        return state.exit_observed || state.cancelled;
    };
    std::thread watchdog;
    try {
        watchdog = std::thread{[child_pid, timeout, &state, &finished]() {
            std::unique_lock lock{state.mutex};
            if (state.changed.wait_for(
                    lock, timeout, finished)) {
                return;
            }
            state.timed_out = true;
            lock.unlock();
            signal_minimap2_child_ownership(child_pid, SIGTERM);
            lock.lock();
            if (state.changed.wait_for(
                    lock, k_minimap2_terminate_grace,
                    [&state]() noexcept { return state.cancelled; })) {
                return;
            }
            lock.unlock();
            // The exact leader may have exited after TERM while descendants
            // still own the spawn-created process group. Always perform the
            // group-scoped KILL escalation at the grace deadline on timeout.
            signal_minimap2_child_ownership(child_pid, SIGKILL);
        }};
    } catch (...) {
        kill_and_reap_minimap2_child(child_pid);
        throw;
    }

    siginfo_t child_info{};
    int observe_status = -1;
    do {
        observe_status = ::waitid(
            P_PID,
            static_cast<id_t>(child_pid),
            &child_info,
            WEXITED | WNOWAIT);
    } while (observe_status != 0 && errno == EINTR);
    if (observe_status != 0
        || child_info.si_pid != child_pid) {
        {
            std::lock_guard lock{state.mutex};
            state.cancelled = true;
        }
        state.changed.notify_one();
        watchdog.join();
        kill_and_reap_minimap2_child(child_pid);
        throw std::runtime_error(
            "waitid failed for exact minimap2 child PID");
    }

    bool timed_out = false;
    {
        std::lock_guard lock{state.mutex};
        state.exit_observed = true;
        timed_out = state.timed_out;
    }
    state.changed.notify_one();
    watchdog.join();

    int child_status = 0;
    pid_t waited = -1;
    do {
        waited = ::waitpid(child_pid, &child_status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited != child_pid) {
        throw std::runtime_error(
            "waitpid failed while reaping exact minimap2 child PID");
    }
    return {
        .status = child_status,
        .timed_out = timed_out,
    };
}

[[nodiscard]] prepared_minimap2_helper_invocation prepare_minimap2_helper(
        const minimap2_receipt_mode mode,
        const int threads,
        const int kmer_size,
        const int window_size,
        const std::uint64_t expected_queries,
        const std::uint64_t expected_index_parts,
        const std::string_view target_or_index,
        const std::string_view query_or_index_output,
        const bool preserve_receipt,
        const std::chrono::milliseconds timeout) {
    const std::string token = unpredictable_minimap2_run_token();
    minimap2_private_workspace workspace =
        make_minimap2_private_workspace(token);
    if (workspace.stdout_fd <= STDERR_FILENO ||
        workspace.stderr_fd <= STDERR_FILENO ||
        workspace.receipt_fd <= STDERR_FILENO ||
        workspace.stdout_fd == k_minimap2_receipt_child_fd ||
        workspace.stderr_fd == k_minimap2_receipt_child_fd ||
        workspace.receipt_fd == k_minimap2_receipt_child_fd) {
        throw std::runtime_error(
            "private minimap2 descriptor collides with the receipt ABI descriptor");
    }

    std::vector<std::string> arguments{
        BIOSEQ_E2E_MINIMAP2_BIN,
        "--bioseq-e2e-mode=" +
            std::string{minimap2_receipt_mode_name(mode)},
        "--bioseq-e2e-run-token=" + token,
        "--bioseq-e2e-receipt-fd=" +
            std::to_string(k_minimap2_receipt_child_fd),
        "--bioseq-e2e-expected-queries=" +
            std::to_string(expected_queries),
        "--bioseq-e2e-expected-index-parts=" +
            std::to_string(expected_index_parts),
        "--bioseq-e2e-temp-root=" + workspace.root.string(),
        "-t" + std::to_string(threads),
        "-k" + std::to_string(kmer_size),
        "-w" + std::to_string(window_size),
    };
    if (mode == minimap2_receipt_mode::index_setup) {
        arguments.push_back("-d" + std::string{query_or_index_output});
        arguments.push_back("--");
        arguments.emplace_back(target_or_index);
    } else {
        arguments.emplace_back("-c");
        arguments.emplace_back("--secondary=no");
        arguments.push_back("--");
        arguments.emplace_back(target_or_index);
        arguments.emplace_back(query_or_index_output);
    }
    return prepared_minimap2_helper_invocation{
        .workspace = std::move(workspace),
        .token = token,
        .mode = mode,
        .expected_queries = expected_queries,
        .expected_index_parts = expected_index_parts,
        .arguments = std::move(arguments),
        .preserve_receipt = preserve_receipt,
        .timeout = timeout,
    };
}

/** Spawn, wait for, and reap exactly one already-prepared helper. */
[[nodiscard]] pending_minimap2_helper_result execute_prepared_minimap2_helper(
        prepared_minimap2_helper_invocation& prepared) {
    std::vector<char*> argv;
    argv.reserve(prepared.arguments.size() + 1u);
    for (std::string& argument : prepared.arguments) {
        argv.push_back(argument.data());
    }
    argv.push_back(nullptr);

    const std::uint64_t parent_child_lifetime_begin_ns =
        minimap2_child_lifetime_clock_now_ns();
    posix_spawn_file_actions_t actions{};
    posix_spawnattr_t attributes{};
    int action_status = ::posix_spawn_file_actions_init(&actions);
    int attribute_status = ::posix_spawnattr_init(&attributes);
    const bool actions_initialized = action_status == 0;
    const bool attributes_initialized = attribute_status == 0;
    const auto add_action = [&action_status](const int status) {
        if (action_status == 0 && status != 0) action_status = status;
    };
    if (action_status == 0) {
        add_action(::posix_spawn_file_actions_adddup2(
            &actions, prepared.workspace.stdout_fd, STDOUT_FILENO));
        add_action(::posix_spawn_file_actions_adddup2(
            &actions, prepared.workspace.stderr_fd, STDERR_FILENO));
        add_action(::posix_spawn_file_actions_adddup2(
            &actions, prepared.workspace.receipt_fd,
            k_minimap2_receipt_child_fd));
        add_action(::posix_spawn_file_actions_addclose(
            &actions, prepared.workspace.stdout_fd));
        add_action(::posix_spawn_file_actions_addclose(
            &actions, prepared.workspace.stderr_fd));
        add_action(::posix_spawn_file_actions_addclose(
            &actions, prepared.workspace.receipt_fd));
    }
    if (attribute_status == 0) {
        attribute_status = ::posix_spawnattr_setpgroup(&attributes, 0);
    }
    if (attribute_status == 0) {
        attribute_status = ::posix_spawnattr_setflags(
            &attributes, POSIX_SPAWN_SETPGROUP);
    }
    if (action_status != 0 || attribute_status != 0) {
        if (actions_initialized) (void)::posix_spawn_file_actions_destroy(&actions);
        if (attributes_initialized) (void)::posix_spawnattr_destroy(&attributes);
        throw std::runtime_error("cannot configure direct minimap2 posix_spawn");
    }

    pid_t child_pid = -1;
    const int spawn_status = ::posix_spawn(
        &child_pid, BIOSEQ_E2E_MINIMAP2_BIN, &actions, &attributes,
        argv.data(), environ);
    (void)::posix_spawn_file_actions_destroy(&actions);
    (void)::posix_spawnattr_destroy(&attributes);
    if (spawn_status != 0 || child_pid <= 0) {
        throw std::runtime_error(
            fmt::format("direct minimap2 posix_spawn failed: {}", spawn_status));
    }

    const minimap2_child_wait_result child_wait =
        wait_for_minimap2_child(child_pid, prepared.timeout);
    const std::uint64_t parent_child_lifetime_end_ns =
        minimap2_child_lifetime_clock_now_ns();

    return pending_minimap2_helper_result{
        .workspace = std::move(prepared.workspace),
        .token = std::move(prepared.token),
        .mode = prepared.mode,
        .expected_queries = prepared.expected_queries,
        .expected_index_parts = prepared.expected_index_parts,
        .child_pid = child_pid,
        .parent_child_lifetime_begin_ns = parent_child_lifetime_begin_ns,
        .parent_child_lifetime_end_ns = parent_child_lifetime_end_ns,
        .child_wait = child_wait,
        .preserve_receipt = prepared.preserve_receipt,
    };
}

[[nodiscard]] pending_minimap2_helper_result execute_minimap2_helper(
        const minimap2_receipt_mode mode,
        const int threads,
        const int kmer_size,
        const int window_size,
        const std::uint64_t expected_queries,
        const std::uint64_t expected_index_parts,
        const std::string_view target_or_index,
        const std::string_view query_or_index_output,
        const bool preserve_receipt,
        const std::chrono::milliseconds timeout) {
    auto prepared = prepare_minimap2_helper(
        mode, threads, kmer_size, window_size, expected_queries,
        expected_index_parts, target_or_index, query_or_index_output,
        preserve_receipt, timeout);
    return execute_prepared_minimap2_helper(prepared);
}

/** Parse and validate helper artifacts after the measured spawn/wait scope. */
[[nodiscard]] minimap2_helper_result finalize_minimap2_helper(
        pending_minimap2_helper_result pending) {
    auto& workspace = pending.workspace;
    const std::string stderr_text = read_minimap2_private_file(
        workspace.stderr_fd, k_minimap2_stderr_max_bytes, "stderr");
    if (pending.child_wait.timed_out) {
        throw std::runtime_error(
            "minimap2 helper exceeded its config-gated deadline; TERM/KILL "
            "escalation completed and the private workspace was reclaimed");
    }
    if (pending.parent_child_lifetime_end_ns
        < pending.parent_child_lifetime_begin_ns) {
        throw std::runtime_error("parent monotonic clock failed around minimap2 child");
    }
    const int child_status = pending.child_wait.status;
    if (WIFSIGNALED(child_status)) {
        throw std::runtime_error(fmt::format(
            "minimap2 helper terminated by signal {}: {}",
            WTERMSIG(child_status), stderr_text));
    }
    if (!WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0) {
        throw std::runtime_error(fmt::format(
            "minimap2 helper returned nonzero status {}: {}",
            WIFEXITED(child_status) ? WEXITSTATUS(child_status) : -1,
            stderr_text));
    }

    minimap2_helper_result result;
    result.stdout_text = read_minimap2_private_file(
        workspace.stdout_fd, k_minimap2_stdout_max_bytes, "stdout");
    const std::string receipt_body = read_minimap2_private_file(
        workspace.receipt_fd, k_minimap2_receipt_max_bytes, "receipt");
    result.receipt = validate_minimap2_receipt(
        receipt_body, pending.token, pending.mode, pending.child_pid,
        pending.expected_queries, pending.expected_index_parts,
        pending.parent_child_lifetime_begin_ns,
        pending.parent_child_lifetime_end_ns, workspace.root);
    minimap2_fnv1a64 artifact_digest;
    for (const unsigned char byte : receipt_body) artifact_digest.append_byte(byte);
    result.artifact_digest = minimap2_hex_u64(artifact_digest.value);
    if (pending.preserve_receipt) {
        result.artifact_path = preserve_minimap2_receipt_artifact(
            pending.token, receipt_body).string();
    }
    return result;
}

[[nodiscard]] minimap2_helper_result run_minimap2_helper(
        const minimap2_receipt_mode mode,
        const int threads,
        const int kmer_size,
        const int window_size,
        const std::uint64_t expected_queries,
        const std::uint64_t expected_index_parts,
        const std::string_view target_or_index,
        const std::string_view query_or_index_output,
        const bool preserve_receipt) {
    return finalize_minimap2_helper(execute_minimap2_helper(
        mode, threads, kmer_size, window_size, expected_queries,
        expected_index_parts, target_or_index, query_or_index_output,
        preserve_receipt,
        std::chrono::duration_cast<std::chrono::milliseconds>(
            k_minimap2_child_timeout)));
}

/// Capture best-effort host resource probes; never used to execute workload helpers.
[[nodiscard]] inline std::string run_resource_probe_capture(
        const std::string& cmd) {
    std::string out;
    std::FILE* pipe = ::popen(cmd.c_str(), "r");
    if (pipe == nullptr) { bioseq_log<logging_level::error>("[bench_e2e] popen failed: {}", cmd); return out; }
    std::array<char, k_popen_buf_bytes> buf;
    std::size_t n;
    while ((n = std::fread(buf.data(), 1, buf.size(), pipe)) > 0) out.append(buf.data(), n);
    const int rc = ::pclose(pipe);
    if (rc != 0) {
        bioseq_log<logging_level::error>("[bench_e2e] command failed (rc={}): {}", rc, cmd);
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

/** @brief Process resource counters captured at one sampling instant. */
struct resource_sample {
    std::uint64_t postgres_rss_kb = 0;
    std::uint64_t minimap2_rss_kb = 0;
    std::uint64_t bench_rss_kb = 0;
    std::uint64_t tmp_available_kb = 0;
    double swap_used_mb = 0.0;
};

[[nodiscard]] inline resource_sample sample_resources() {
    resource_sample out{};

    const std::string rss = run_resource_probe_capture(
        "ps -axo comm=,rss= | awk '"
        "$1 ~ /postgres/ {pg += $2} "
        "$1 ~ /minimap2/ {mm += $2} "
        "$1 ~ /bioseq_benchmark_e2e/ {bench += $2} "
        "END {printf \"%llu %llu %llu\\n\", pg, mm, bench}'");
    unsigned long long pg = 0, mm = 0, bench = 0;
    if (std::sscanf(rss.c_str(), "%llu %llu %llu", &pg, &mm, &bench) == 3) {
        out.postgres_rss_kb = static_cast<std::uint64_t>(pg);
        out.minimap2_rss_kb = static_cast<std::uint64_t>(mm);
        out.bench_rss_kb = static_cast<std::uint64_t>(bench);
    }

    out.tmp_available_kb = parse_u64_prefix(
        run_resource_probe_capture("df -k /tmp | awk 'NR==2 {print $4}'"));
#if defined(__APPLE__)
    out.swap_used_mb = parse_double_prefix(run_resource_probe_capture(
        "sysctl -n vm.swapusage | awk "
        "'{for (i=1; i<=NF; ++i) if ($i == \"used\") {v=$(i+2); "
        "gsub(\"M\", \"\", v); print v; exit}}'"));
#elif defined(__linux__)
    out.swap_used_mb = parse_double_prefix(run_resource_probe_capture(
        "awk '/^SwapTotal:/ {total=$2} /^SwapFree:/ {free=$2} "
        "END {if (total >= free) printf \"%.3f\\n\", (total - free) / 1024.0; "
        "else print 0}' /proc/meminfo"));
#else
    out.swap_used_mb = 0.0;
#endif
    return out;
}

/** @brief Samples process resources for the lifetime of a measured scope. */
class scoped_resource_monitor {
public:
    scoped_resource_monitor() {
        record(sample_resources());
        worker_ = std::thread([this]() {
            while (!stop_.load(std::memory_order_relaxed)) {
                record(sample_resources());
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
        });
    }

    scoped_resource_monitor(const scoped_resource_monitor&) = delete;
    scoped_resource_monitor& operator=(const scoped_resource_monitor&) = delete;

    ~scoped_resource_monitor() {
        (void)stop();
    }

    [[nodiscard]] resource_sample stop() {
        stop_.store(true, std::memory_order_relaxed);
        if (worker_.joinable()) {
            worker_.join();
        }
        record(sample_resources());
        return peaks_;
    }

private:
    void record(const resource_sample& s) noexcept {
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
    resource_sample peaks_{};
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

/** @brief One parsed CIGAR operation used by benchmark-side replay. */
struct benchmark_cigar_operation {
    std::uint64_t length = 0;
    char code = '\0';
};

[[nodiscard]] bool parse_benchmark_cigar(
        std::string_view cigar,
        std::vector<benchmark_cigar_operation>& operations) {
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
        operations.push_back(benchmark_cigar_operation{
            .length = length,
            .code = character,
        });
        length = 0;
        in_number = false;
    }
    return !operations.empty() && !in_number;
}

[[nodiscard]] bool is_valid_benchmark_cigar(std::string_view cigar) {
    std::vector<benchmark_cigar_operation> operations;
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
        const e2e_input& in) {
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
        const e2e_input& in,
        const e2e_query_truth& query,
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
        [&result, forward]<sequence_alphabet p_alphabet>() {
            for (auto it = forward.rbegin(); it != forward.rend(); ++it) {
                p_alphabet letter{};
                letter.assign_char(*it);
                result.push_back(seqan3::complement(letter).to_char());
            }
        });
    return result;
}

[[nodiscard]] bool replay_benchmark_cigar(
        std::string_view cigar,
        const e2e_input& in,
        const e2e_query_truth& query,
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

    std::vector<benchmark_cigar_operation> operations;
    if (!parse_benchmark_cigar(cigar, operations)) return false;
    std::size_t query_cursor = 0;
    std::size_t target_cursor = 0;
    for (const benchmark_cigar_operation operation : operations) {
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

void apply_designed_indel_cigar_mutation(e2e_query_truth& query,
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
    constexpr std::size_t k_delete_len = 8u;
    query.bases.erase(del_pos, k_delete_len);

    static constexpr std::string_view k_insert_a = "ACGTACGTACGTACGT";
    static constexpr std::string_view k_insert_b = "TGCATGCATGCATGCA";
    const std::string_view insert = (ordinal % 2u == 0u) ? k_insert_a : k_insert_b;
    const std::size_t ins_pos = (query.bases.size() * 2u) / 3u;
    query.bases.insert(ins_pos, insert);
}

void apply_designed_many_gap_cigar_mutation(e2e_query_truth& query) {
    static constexpr std::array<std::size_t, 20> k_delete_positions{
        40u, 80u, 120u, 160u, 200u, 240u, 280u, 320u, 360u, 400u,
        440u, 480u, 520u, 560u, 600u, 640u, 680u, 720u, 760u, 800u};
    if (query.bases.size() <= k_delete_positions.back()) {
        std::fprintf(
            stderr,
            "[bench_e2e] designed many-gap CIGAR case requires query length > %zu "
            "(qid=%d len=%zu)\n",
            k_delete_positions.back(),
            query.id,
            query.bases.size());
        std::fflush(stderr);
        std::abort();
    }
    for (auto it = k_delete_positions.rbegin(); it != k_delete_positions.rend(); ++it) {
        query.bases.erase(*it, 1u);
    }
}

inline constexpr std::string_view k_ordered_query_oracle_digest_domain =
    "bioseq.e2e.ordered-query-primary-count.v2";
inline constexpr std::string_view k_ordered_query_mapq_digest_domain =
    "bioseq.e2e.ordered-query-mapq-diagnostic.v1";

void initialize_ordered_query_oracle(
        const e2e_input& in,
        e2e_output& out) {
    out.ordered_query_oracle.clear();
    out.ordered_query_oracle.reserve(in.queries.size());
    for (std::size_t index = 0; index < in.queries.size(); ++index) {
        const e2e_query_truth& query = in.queries[index];
        if (query.id < 0
            || static_cast<std::size_t>(query.id) != index) {
            std::fputs(
                "[bench_e2e] ORDERED-QUERY-FATAL query identities must be "
                "the exact canonical input order 0..N-1\n",
                stderr);
            std::fflush(stderr);
            std::abort();
        }
        out.ordered_query_oracle.push_back(e2e_query_oracle_observation{
            .query_id = query.id,
            .primary_match_count = 0,
            .mapq = -1,
        });
    }
    out.ordered_query_oracle_available = 0;
}

void finalize_ordered_query_oracle(
        const e2e_input& in,
        e2e_output& out) {
    (void)in;
    bioseq::common::sha256_hasher count_hasher;
    count_hasher.update(std::as_bytes(std::span{
        k_ordered_query_oracle_digest_domain.data(),
        k_ordered_query_oracle_digest_domain.size()}));
    count_hasher.update_little_endian<std::uint64_t>(
        static_cast<std::uint64_t>(out.ordered_query_oracle.size()));
    bioseq::common::sha256_hasher mapq_hasher;
    mapq_hasher.update(std::as_bytes(std::span{
        k_ordered_query_mapq_digest_domain.data(),
        k_ordered_query_mapq_digest_domain.size()}));
    mapq_hasher.update_little_endian<std::uint64_t>(
        static_cast<std::uint64_t>(out.ordered_query_oracle.size()));
    for (const e2e_query_oracle_observation& observation :
         out.ordered_query_oracle) {
        count_hasher.update_little_endian<std::uint64_t>(
            static_cast<std::uint64_t>(observation.query_id));
        count_hasher.update_little_endian<std::uint64_t>(
            static_cast<std::uint64_t>(observation.primary_match_count));
        mapq_hasher.update_little_endian<std::uint64_t>(
            static_cast<std::uint64_t>(observation.query_id));
        mapq_hasher.update_little_endian<std::uint64_t>(
            observation.mapq < 0
                ? (std::numeric_limits<std::uint64_t>::max)()
                : static_cast<std::uint64_t>(observation.mapq));
    }
    const bioseq::common::sha256_digest digest = count_hasher.finalize();
    const bioseq::common::sha256_digest mapq_digest = mapq_hasher.finalize();
    for (std::size_t word = 0; word < out.ordered_query_oracle_sha256_words.size();
         ++word) {
        const std::size_t offset = word * 4u;
        out.ordered_query_oracle_sha256_words[word] =
            static_cast<std::uint32_t>(
                std::to_integer<std::uint8_t>(digest[offset])) << 24u
            | static_cast<std::uint32_t>(
                std::to_integer<std::uint8_t>(digest[offset + 1u])) << 16u
            | static_cast<std::uint32_t>(
                std::to_integer<std::uint8_t>(digest[offset + 2u])) << 8u
            | static_cast<std::uint32_t>(
                std::to_integer<std::uint8_t>(digest[offset + 3u]));
        out.ordered_query_mapq_sha256_words[word] =
            static_cast<std::uint32_t>(
                std::to_integer<std::uint8_t>(mapq_digest[offset])) << 24u
            | static_cast<std::uint32_t>(
                std::to_integer<std::uint8_t>(mapq_digest[offset + 1u])) << 16u
            | static_cast<std::uint32_t>(
                std::to_integer<std::uint8_t>(mapq_digest[offset + 2u])) << 8u
            | static_cast<std::uint32_t>(
                std::to_integer<std::uint8_t>(mapq_digest[offset + 3u]));
    }
    out.ordered_query_oracle_sha256 = bioseq::common::to_hex(digest);
    out.ordered_query_mapq_sha256 = bioseq::common::to_hex(mapq_digest);
    out.ordered_query_oracle_available = 1;
}

// bioseq: the custom-scan projects one row per matched target row. Columns are
// [header, tstart, rev]; ORDER BY header. At fragment_count=1 there is exactly
// one target row.
void parse_bioseq_rows(const it::rowset_t& rows, const e2e_input& in, e2e_output& out) {
    out.correctness_evaluated = 1;
    out.expected_match_count = 1;
    out.match_count = static_cast<std::int64_t>(rows.size());   // matched target rows
    if (rows.size() != 1 || in.queries.empty()) { out.found = false; return; }
    const it::row_t& r0 = rows.front();                          // [header, tstart, rev]
    const std::size_t expected_columns = (in.cigar_projection ? 8u : 3u) +
                                         (in.mapq_projection ? 1u : 0u);
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
    out.position_offset_available = std::isfinite(out.abs_offset_bp);
    out.correct_match_count =
        out.abs_offset_bp < static_cast<double>(k_position_tolerance_bp) ? 1 : 0;
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
    if (in.mapq_projection && r0.back().has_value()) {
        const auto mapq = parse_i64_exact(*r0.back());
        if (mapq.has_value() && *mapq >= 0
            && *mapq <= k_max_mapping_quality) {
            out.mapq_result_count = 1;
        }
    }
    out.found = out.correct_match_count == 1 && out.correct_strand_count == 1
        && (!in.cigar_projection || out.cigar_replay_count == 1)
        && (!in.mapq_projection || out.mapq_result_count == 1);
}

void parse_bioseq_batch_rows(
        const it::rowset_t& rows,
        const e2e_input& in,
        e2e_output& out,
        const std::int64_t target_coordinate_origin = 0)
{
    out.correctness_evaluated = 1;
    std::vector<std::uint8_t> row_counts(in.queries.size(), 0);
    std::vector<std::optional<double>> offsets(in.queries.size());
    const auto abort_structural_row_corruption = [] [[noreturn]] (const char* reason) {
        std::fprintf(stderr,
            "[bench_e2e] BIOSEQ-RESULT-STRUCTURAL-CORRUPTION %s\n", reason);
        std::fflush(stderr);
        std::abort();
    };
    if (rows.size() > in.queries.size()) {
        abort_structural_row_corruption(
            "SQL result returned more rows than the submitted query identity set");
    }

    for (const it::row_t& row : rows) {
        const std::size_t expected_columns = (in.cigar_projection ? 9u : 4u) +
                                             (in.mapq_projection ? 1u : 0u);
        if (row.size() != expected_columns) {
            abort_structural_row_corruption("SQL result has the wrong column count");
        }
        if (!row[0].has_value() || !row[2].has_value()
            || !row[in.cigar_projection ? 6u : 3u].has_value()) {
            abort_structural_row_corruption("SQL result has a NULL required identity field");
        }
        const auto qid_value = parse_i64_exact(*row[0]);
        const auto target_start_value = parse_i64_exact(*row[2]);
        if (!qid_value.has_value() || !target_start_value.has_value()
            || *qid_value < 0
            || static_cast<std::size_t>(*qid_value) >= in.queries.size()) {
            abort_structural_row_corruption(
                "SQL result has an ill-typed or unknown query identity");
        }
        const auto qid = static_cast<std::size_t>(*qid_value);
        if (row_counts[qid] != 0) {
            abort_structural_row_corruption(
                "SQL result returned a duplicate query identity");
        }
        ++row_counts[qid];
        ++out.ordered_query_oracle[qid].primary_match_count;
        const std::int64_t target_start = *target_start_value;
        if (target_coordinate_origin < 0
            || in.queries[qid].truth_target_start
                < target_coordinate_origin) {
            abort_structural_row_corruption(
                "benchmark target-coordinate origin is inconsistent with query truth");
        }
        const std::int64_t expected_local_target_start =
            in.queries[qid].truth_target_start - target_coordinate_origin;
        const double offset = static_cast<double>(
            std::llabs(target_start - expected_local_target_start));
        offsets[qid] = offset;
        const bool coordinate_correct =
            offset < static_cast<double>(k_position_tolerance_bp);
        const auto is_reverse_value = parse_pg_bool_exact(
            row[in.cigar_projection ? 6u : 3u].value());
        if (!is_reverse_value.has_value()) {
            abort_structural_row_corruption(
                "SQL result has an ill-typed strand value");
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
        if (in.mapq_projection && row.back().has_value()) {
            const auto mapq = parse_i64_exact(*row.back());
            if (!mapq.has_value()) {
                abort_structural_row_corruption(
                    "SQL result has an ill-typed MAPQ value");
            }
            if (*mapq >= 0 && *mapq <= k_max_mapping_quality) {
                ++out.mapq_result_count;
                if (in.paired_minimap2_oracle) {
                    out.ordered_query_oracle[qid].mapq = *mapq;
                }
            } else {
                abort_structural_row_corruption(
                    "SQL result has an out-of-range MAPQ value");
            }
        } else if (in.mapq_projection) {
            abort_structural_row_corruption("SQL result has a NULL MAPQ value");
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
        if (!target_end.has_value() || !query_start.has_value()
            || !query_end.has_value() || !has_cigar_value.has_value()) {
            abort_structural_row_corruption(
                "SQL result has a NULL or ill-typed CIGAR span field");
        }
        const bool has_cigar = has_cigar_value.value_or(false);
        if (!row[8].has_value()) {
            abort_structural_row_corruption("SQL result has a NULL CIGAR value");
        }
        if (!has_cigar) {
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
        if (!offsets[i].has_value()) continue;
        if (row_counts[i] == 1
            && *offsets[i] < static_cast<double>(k_position_tolerance_bp)) {
            ++out.correct_match_count;
        }
        out.max_abs_offset_bp = std::max(out.max_abs_offset_bp, *offsets[i]);
    }
    out.position_offset_available = !offsets.empty()
        && std::ranges::all_of(offsets, [](const auto& offset) noexcept {
            return offset.has_value() && std::isfinite(*offset);
        });
    out.exact_primary_count_accepted = std::ranges::all_of(row_counts,
            [](const std::uint8_t count) noexcept { return count == 1; })
        ? 1 : 0;
    out.found = out.exact_primary_count_accepted == 1
        && out.correct_match_count == out.expected_match_count
        && out.correct_strand_count == out.expected_match_count
        && (!in.cigar_projection
            || (out.correct_cigar_count == out.expected_match_count
                && out.cigar_replay_count == out.expected_match_count))
        && (!in.mapq_projection
            || out.mapq_result_count == out.expected_match_count);
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

/** @brief Duplicate-detection state for one pipeline stats document. */
struct pipeline_stats_parse_state {
    std::set<std::string> seen_fields;
    bool group_found = false;
    bool invalid = false;
};

[[noreturn]] void abort_pipeline_stats_capture(std::string_view reason) {
    std::fprintf(
        stderr,
        "[bench_e2e] PIPELINE-VALIDATION-CAPTURE-FAIL %.*s\n",
        static_cast<int>(reason.size()),
        reason.data());
    std::fflush(stderr);
    std::abort();
}

void assign_pipeline_explain_metric(
        std::string_view key,
        simdjson::dom::element value,
        e2e_output& out,
        pipeline_stats_parse_state& state)
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

void parse_pipeline_stats_object(
        simdjson::dom::element element,
        e2e_output& out,
        pipeline_stats_parse_state& state)
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
        e2e_output& out,
        pipeline_stats_parse_state& state)
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
        if (field.key == "Bioseq Execution Statistics") {
            if (state.group_found) {
                state.invalid = true;
                continue;
            }
            state.group_found = true;
            parse_pipeline_stats_object(field.value, out, state);
            continue;
        }
        walk_pipeline_explain_json(field.value, out, state);
    }
}

[[nodiscard]] bool pipeline_stats_fields_complete(
        const e2e_output& out) noexcept
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
        e2e_output& out)
{
    // This is a second, untimed statement. Preserve the stats for
    // diagnostics, but never present it as stats of the measured statement.
    out.pipeline_stats_attached_to_measurement = 0;
    const it::rowset_t explain_rows = it::query_rows(
        c,
        "EXPLAIN (ANALYZE, VERBOSE, FORMAT JSON) " + align_sql);
    if (explain_rows.empty() || explain_rows.front().empty()
        || !explain_rows.front()[0].has_value()) {
        abort_pipeline_stats_capture(
            "canonical pipeline EXPLAIN returned no JSON document");
    }
    simdjson::dom::parser parser;
    simdjson::dom::element doc;
    const std::string& json_text = *explain_rows.front()[0];
    if (parser.parse(json_text).get(doc) != simdjson::SUCCESS) {
        abort_pipeline_stats_capture(
            "canonical pipeline EXPLAIN JSON failed to parse");
    }
    pipeline_stats_parse_state state{};
    walk_pipeline_explain_json(doc, out, state);
    if (!state.group_found || state.invalid
        || !pipeline_stats_fields_complete(out)) {
        abort_pipeline_stats_capture(
            "canonical pipeline EXPLAIN validation is missing, duplicated, or ill-typed");
    }
    out.pipeline_stats_available = 1;
}

/** @brief Identity of the most recently published alignment snapshot. */
struct alignment_snapshot_identity {
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

[[nodiscard]] alignment_snapshot_identity capture_prior_alignment_identity(
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
        abort_pipeline_stats_capture(
            "prior alignment snapshot identity has an invalid row shape");
    }
    const auto execution_id = parse_snapshot_i64(rows.front()[0]);
    const auto backend_pid = parse_snapshot_i64(rows.front()[1]);
    const auto database_oid = parse_snapshot_i64(rows.front()[2]);
    if (!execution_id.has_value() || !backend_pid.has_value()
        || !database_oid.has_value() || *backend_pid == 0
        || *database_oid == 0 || !rows.front()[3].has_value()
        || rows.front()[3]->empty()) {
        abort_pipeline_stats_capture(
            "prior alignment snapshot identity is missing or ill-typed");
    }
    return alignment_snapshot_identity{
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
    e2e_output::k_alignment_snapshot_column_count> k_alignment_snapshot_column_names{
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
    "is_completed",
    "is_partial",
    "is_fully_drained",
    "requested_backend",
    "actual_backend",
    "requested_target_index_source",
    "actual_target_index_source",
    "persisted_generation_id",
    "persisted_source_identity",
    "has_mixed_persisted_identity",
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
    "has_operational_identity_conflict",
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
	"are_stats_saturated",
	"persisted_source_parent_rows_read",
	"persisted_source_catalog_rows_read",
	"persisted_source_catalog_index_scans",
	"persisted_source_fragment_rows_read",
	"persisted_source_decoded_bp",
	"persisted_pair_index_conversion_calls",
	"persisted_segment_rebase_calls",
	"cold_index_preparation",
	"simd_execution_stats",
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

/** @brief Stable cache identity fields required by the qualification gate. */
struct gpu_cache_identity_gate_stats {
    std::string_view device_uuid;
    std::string_view driver;
    std::string_view policy;
    std::string_view seed_sha256;
    std::int64_t seed_size = -1;
    std::string_view manifest_checksum;
    std::int64_t hit = -1;
};

[[nodiscard]] constexpr bool gpu_cache_identity_stats_valid(
        const gpu_cache_identity_gate_stats& stats) noexcept {
#if defined(__linux__)
    if (stats.policy == "native_vulkan") {
        return nonzero_fixed_lower_hex(stats.device_uuid, 32) && !stats.driver.empty()
            && stats.seed_sha256 == std::string_view{
                "0000000000000000000000000000000000000000000000000000000000000000"}
            && stats.seed_size == 0 && stats.manifest_checksum == "0000000000000000"
            && stats.hit == 0;
    }
#endif
    return nonzero_fixed_lower_hex(stats.device_uuid, 32)
        && !stats.driver.empty()
        && stats.policy == "immutable_preseed_all_hit"
        && nonzero_fixed_lower_hex(stats.seed_sha256, 64)
        && stats.seed_size > 0
        && nonzero_fixed_lower_hex(stats.manifest_checksum, 16)
        && stats.hit == 1;
}

/** @brief Runtime cache counters required by the qualification gate. */
struct gpu_cache_runtime_gate_stats {
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

[[nodiscard]] constexpr bool gpu_cache_runtime_stats_valid(
        const gpu_cache_runtime_gate_stats& stats,
        std::string_view policy = "immutable_preseed_all_hit") noexcept {
#if defined(__linux__)
    if (policy == "native_vulkan") {
        return stats.declared_seed_size == 0 && stats.loads == 0 && stats.seed_bytes == 0
            && stats.load_ns == 0 && stats.creation_ns > 0 && stats.pipeline_creates > 0
            && stats.timestamp_failures == 0 && stats.backend_declines == 0
            && stats.backend_fallbacks == 0;
    }
#endif
    return policy == "immutable_preseed_all_hit" && stats.declared_seed_size > 0
        && stats.loads == 1
        && stats.seed_bytes == stats.declared_seed_size
        && stats.load_ns > 0
        && stats.creation_ns > 0
        && stats.timestamp_failures == 0
        && stats.pipeline_creates > 0
        && stats.backend_declines == 0
        && stats.backend_fallbacks == 0;
}

consteval bool gpu_cache_gate_contract_self_test() {
    constexpr gpu_cache_identity_gate_stats valid_identity{
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
            gpu_cache_identity_gate_stats stats,
            T gpu_cache_identity_gate_stats::* member,
            T value) constexpr {
        stats.*member = value;
        return stats;
    };
    /** @brief One cache identity gate fixture and expected decision. */
    struct identity_case {
        std::string_view name;
        gpu_cache_identity_gate_stats stats;
        bool expected;
    };
    constexpr auto identity_cases = std::to_array<identity_case>({
        {"valid immutable hit", valid_identity, true},
        {"isolated policy rejected", changed_identity(valid_identity,
            &gpu_cache_identity_gate_stats::policy,
            std::string_view{"isolated_miss_diagnostic"}), false},
        {"not-applicable policy rejected", changed_identity(valid_identity,
            &gpu_cache_identity_gate_stats::policy,
            std::string_view{"not_applicable"}), false},
        {"mixed policy rejected", changed_identity(valid_identity,
            &gpu_cache_identity_gate_stats::policy,
            std::string_view{"mixed_invalid"}), false},
        {"cache miss rejected", changed_identity(valid_identity,
            &gpu_cache_identity_gate_stats::hit, std::int64_t{0}), false},
        {"unset hit rejected", changed_identity(valid_identity,
            &gpu_cache_identity_gate_stats::hit, std::int64_t{-1}), false},
        {"mixed hit rejected", changed_identity(valid_identity,
            &gpu_cache_identity_gate_stats::hit, std::int64_t{2}), false},
        {"zero UUID rejected", changed_identity(valid_identity,
            &gpu_cache_identity_gate_stats::device_uuid,
            std::string_view{"00000000000000000000000000000000"}), false},
        {"malformed UUID rejected", changed_identity(valid_identity,
            &gpu_cache_identity_gate_stats::device_uuid,
            std::string_view{"A1111111111111111111111111111111"}), false},
        {"empty driver rejected", changed_identity(valid_identity,
            &gpu_cache_identity_gate_stats::driver,
            std::string_view{}), false},
        {"zero SHA rejected", changed_identity(valid_identity,
            &gpu_cache_identity_gate_stats::seed_sha256,
            std::string_view{
                "00000000000000000000000000000000"
                "00000000000000000000000000000000"}), false},
        {"malformed SHA rejected", changed_identity(valid_identity,
            &gpu_cache_identity_gate_stats::seed_sha256,
            std::string_view{"1"}), false},
        {"zero seed size rejected", changed_identity(valid_identity,
            &gpu_cache_identity_gate_stats::seed_size,
            std::int64_t{0}), false},
        {"zero manifest rejected", changed_identity(valid_identity,
            &gpu_cache_identity_gate_stats::manifest_checksum,
            std::string_view{"0000000000000000"}), false},
        {"malformed manifest rejected", changed_identity(valid_identity,
            &gpu_cache_identity_gate_stats::manifest_checksum,
            std::string_view{"000000000000123G"}), false},
    });
    for (const auto& test : identity_cases) {
        (void)test.name;
        if (gpu_cache_identity_stats_valid(test.stats)
                != test.expected) {
            return false;
        }
    }

    constexpr gpu_cache_runtime_gate_stats valid_runtime{
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
            gpu_cache_runtime_gate_stats stats,
            std::int64_t gpu_cache_runtime_gate_stats::* member,
            const std::int64_t value) constexpr {
        stats.*member = value;
        return stats;
    };
    /** @brief One runtime cache gate fixture and expected decision. */
    struct runtime_case {
        std::string_view name;
        gpu_cache_runtime_gate_stats stats;
        bool expected;
    };
    constexpr auto runtime_cases = std::to_array<runtime_case>({
        {"valid runtime validation", valid_runtime, true},
        {"zero declared bytes rejected", changed_runtime(valid_runtime,
            &gpu_cache_runtime_gate_stats::declared_seed_size, 0), false},
        {"zero loads rejected", changed_runtime(valid_runtime,
            &gpu_cache_runtime_gate_stats::loads, 0), false},
        {"multiple loads rejected", changed_runtime(valid_runtime,
            &gpu_cache_runtime_gate_stats::loads, 2), false},
        {"zero seed bytes rejected", changed_runtime(valid_runtime,
            &gpu_cache_runtime_gate_stats::seed_bytes, 0), false},
        {"mismatched seed bytes rejected", changed_runtime(valid_runtime,
            &gpu_cache_runtime_gate_stats::seed_bytes, 63), false},
        {"zero load timing rejected", changed_runtime(valid_runtime,
            &gpu_cache_runtime_gate_stats::load_ns, 0), false},
        {"zero creation timing rejected", changed_runtime(valid_runtime,
            &gpu_cache_runtime_gate_stats::creation_ns, 0), false},
        {"timestamp failure rejected", changed_runtime(valid_runtime,
            &gpu_cache_runtime_gate_stats::timestamp_failures, 1), false},
        {"zero pipeline creates rejected", changed_runtime(valid_runtime,
            &gpu_cache_runtime_gate_stats::pipeline_creates, 0), false},
        {"backend decline rejected", changed_runtime(valid_runtime,
            &gpu_cache_runtime_gate_stats::backend_declines, 1), false},
        {"backend fallback rejected", changed_runtime(valid_runtime,
            &gpu_cache_runtime_gate_stats::backend_fallbacks, 1), false},
    });
    for (const auto& test : runtime_cases) {
        (void)test.name;
        if (gpu_cache_runtime_stats_valid(test.stats)
                != test.expected) {
            return false;
        }
    }
#if defined(__linux__)
    auto native_identity = valid_identity;
    native_identity.policy = "native_vulkan";
    native_identity.seed_sha256 =
        "0000000000000000000000000000000000000000000000000000000000000000";
    native_identity.seed_size = 0;
    native_identity.manifest_checksum = "0000000000000000";
    native_identity.hit = 0;
    if (!gpu_cache_identity_stats_valid(native_identity)) return false;
    native_identity.hit = 1;
    if (gpu_cache_identity_stats_valid(native_identity)) return false;
    native_identity.hit = 0;
    native_identity.seed_size = 1;
    if (gpu_cache_identity_stats_valid(native_identity)) return false;
    auto native_runtime = valid_runtime;
    native_runtime.declared_seed_size = 0;
    native_runtime.loads = 0;
    native_runtime.seed_bytes = 0;
    native_runtime.load_ns = 0;
    if (!gpu_cache_runtime_stats_valid(native_runtime, "native_vulkan")) return false;
    native_runtime.pipeline_creates = 0;
    if (gpu_cache_runtime_stats_valid(native_runtime, "native_vulkan")) return false;
    native_runtime.pipeline_creates = 1;
    native_runtime.backend_fallbacks = 1;
    if (gpu_cache_runtime_stats_valid(native_runtime, "native_vulkan")) return false;
#endif
    return true;
}

static_assert(gpu_cache_gate_contract_self_test());

inline constexpr auto k_cold_index_stats_keys =
    std::to_array<std::string_view>({
        "schema", "revision", "timing_semantics",
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
        "are_stats_saturated",
    });
static_assert(k_cold_index_stats_keys.size() == 59);

inline constexpr auto k_minimizer_fragment_reader_stats_keys =
    std::to_array<std::string_view>({
        "schema", "revision", "execution_id", "backend_pid", "database_oid",
        "grouped_soa_reader_capacity_accounting_passes",
        "grouped_soa_reader_global_position_storage_preparations",
        "grouped_soa_reader_global_position_storage_allocations",
        "grouped_soa_reader_position_append_growth_reservations",
        "grouped_soa_reader_retained_position_count",
        "grouped_soa_reader_retained_position_capacity",
        "grouped_soa_reader_directory_capacity_bytes",
        "grouped_soa_reader_query_hash_capacity_bytes",
        "grouped_soa_reader_query_record_capacity_bytes",
        "grouped_soa_reader_ordinal_route_capacity_bytes",
        "grouped_soa_reader_selected_extent_capacity_bytes",
        "grouped_soa_reader_current_selected_control_capacity_bytes",
        "grouped_soa_reader_global_position_capacity_bytes",
        "grouped_soa_reader_control_payload_capacity_bytes",
        "grouped_soa_reader_owned_capacity_bytes",
        "grouped_soa_reader_admitted_bytes",
        "grouped_soa_reader_exact_position_image_checks",
        "grouped_soa_reader_exact_position_image_failures",
        "grouped_soa_reader_retained_layout_checks",
        "grouped_soa_reader_retained_layout_failures",
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
        "fragment_native_peak_block_stats_capacity_bytes",
        "fragment_native_occurrence_allocations",
        "fragment_native_query_plan_allocations",
        "fragment_native_query_radix_allocations",
        "fragment_native_occurrence_count_allocations",
        "fragment_native_block_layout_allocations",
        "fragment_native_block_stats_allocations",
        "fragment_native_query_occurrences",
        "fragment_native_unique_query_plans",
        "fragment_native_packed_occurrence_waves",
        "fragment_native_wide_occurrence_waves",
        "are_fragment_native_stats_saturated",
        "are_grouped_soa_reader_memory_stats_saturated",
        "are_stats_saturated",
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
        "fragment_native_peak_query_group_offset_capacity_bytes",
        "fragment_native_peak_query_membership_filter_capacity_bytes",
        "fragment_native_query_group_offset_allocations",
        "fragment_native_query_membership_filter_allocations",
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
static_assert(k_minimizer_fragment_reader_stats_keys.size() == 140);

inline constexpr auto k_grouped_soa_provider_stats_counter_keys =
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
static_assert(k_grouped_soa_provider_stats_counter_keys.size() == 50);

inline constexpr auto k_fragment_native_stats_counter_keys =
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
        "fragment_native_peak_block_stats_capacity_bytes",
        "fragment_native_occurrence_allocations",
        "fragment_native_query_plan_allocations",
        "fragment_native_query_group_offset_allocations",
        "fragment_native_query_membership_filter_allocations",
        "fragment_native_query_radix_allocations",
        "fragment_native_occurrence_count_allocations",
        "fragment_native_block_layout_allocations",
        "fragment_native_block_stats_allocations",
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
static_assert(k_fragment_native_stats_counter_keys.size() == 62);

inline constexpr auto k_compact_direct_reader_stats_counter_keys =
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
static_assert(k_compact_direct_reader_stats_counter_keys.size() == 15);

inline constexpr auto k_hash_major_index_reader_stats_keys =
    std::to_array<std::string_view>({
        "schema", "revision", "execution_id", "backend_pid", "database_oid",
        "format_revision", "sessions", "generation_id", "source_identity",
        "source_content_crc64", "compatibility_digest", "directory_rows",
        "directory_bytes", "control_rows", "control_bytes", "posting_rows",
        "posting_bytes", "directory_row_crc_receipts",
        "directory_stream_crc_receipts", "control_row_crc_receipts",
        "control_block_crc_receipts", "control_semantic_receipts",
        "posting_row_crc_receipts", "posting_ranges_delivered",
        "posting_range_crc_receipts", "posting_semantic_receipts",
        "authorized_posting_bytes", "query_count", "selected_block_count",
        "accepted_hash_count",
        "retained_position_count", "directory_stage_peak_bytes",
        "control_stage_peak_bytes", "posting_stage_peak_bytes",
        "peak_admitted_bytes", "retained_owner_bytes", "admitted_bytes",
        "assembly_calls", "pair_index_conversion_calls",
        "segment_rebase_calls", "recompute_calls",
        "generic_source_fallback_calls", "backend_fallbacks", "failures",
        "is_saturated", "has_operational_identity_conflict",
        "are_stats_saturated",
    });
static_assert(k_hash_major_index_reader_stats_keys.size() == 47);

inline constexpr auto k_hash_major_ready_audit_keys =
    std::to_array<std::string_view>({
        "schema", "revision", "layout", "database_oid",
        "main_relfilenode", "column_attno", "query_key", "table_name",
        "index_name", "source", "head", "manifest", "metadata", "streams",
    });
inline constexpr auto k_hash_major_ready_source_keys =
    std::to_array<std::string_view>({
        "sequence_rowno", "locator_revision", "bucket_idx", "shard_idx",
        "num_fragments", "total_bp_size", "first_sub_row_no", "source_epoch",
        "padding_letters", "alphabet_code", "storage_unit_byte_width",
        "source_content_crc64", "source_identity",
    });
inline constexpr auto k_hash_major_ready_head_keys =
    std::to_array<std::string_view>({
        "generation_id", "query_key", "source_epoch", "source_identity",
        "source_content_crc64", "compatibility_digest",
        "manifest_address_bound_row_crc64", "manifest_payload_crc64",
        "head_crc64", "dense_seal_present",
    });
inline constexpr auto k_hash_major_ready_manifest_keys =
    std::to_array<std::string_view>({
        "generation_id", "source_epoch", "source_identity",
        "source_content_crc64", "target_length", "unique_hash_count",
        "total_posting_count", "compatibility_digest",
        "authenticated_directory_crc64", "generation_crc64",
        "total_control_bytes", "total_posting_bytes",
        "directory_entry_count", "compatibility",
    });
inline constexpr auto k_hash_major_ready_compatibility_keys =
    std::to_array<std::string_view>({
        "hash_scheme_identity", "hash_scheme_revision", "alphabet_code",
        "kmer_size", "window_size", "hash_width_bytes",
        "stored_posting_width_bytes", "position_encoding_revision",
        "hash_semantics_revision", "count_semantics_revision",
        "producer_backend_identity", "producer_backend_revision",
        "controls_per_block", "hash_seed",
    });
inline constexpr auto k_hash_major_ready_metadata_keys =
    std::to_array<std::string_view>({
        "head_rows", "head_bytes", "manifest_rows", "manifest_bytes",
    });
inline constexpr auto k_hash_major_ready_stream_keys =
    std::to_array<std::string_view>({
        "kind", "element_count", "logical_bytes", "row_count",
        "stream_crc64", "exact_catalog_rows", "exact_catalog_bytes",
        "exact_catalog_stream_crc64",
    });

inline constexpr auto k_simd_execution_stats_keys =
    std::to_array<std::string_view>({
        "schema", "revision", "requested_minimizer_algorithm",
        "observed_minimizer_algorithm", "requested_chaining_algorithm",
        "observed_chaining_algorithm", "requested_base_alignment_algorithm",
        "observed_base_alignment_algorithm", "requested_policy", "observed_policy",
        "requested_alignment_algorithm", "observed_alignment_algorithm",
        "downstream_active_wall_available", "downstream_active_wall_ns",
        "downstream_active_wall_invocation_count",
        "downstream_active_wall_unavailable_invocation_count",
        "downstream_work_ns", "downstream_work_saturated",
        "mapq_execution_available", "mapq_executed", "mapq_execution_conflict",
        "nthash_vector_batches",
        "nthash_resident_vector_stream_kernel_calls",
        "simd_chaining_exact_policy_calls",
        "simd_chaining_bounded_policy_calls",
        "simd_chaining_bounded_score_batches",
        "simd_chaining_bounded_cutoff_stops",
        "simd_chaining_bounded_candidates_skipped",
        "simd_chaining_policy_rejections",
        "are_simd_chaining_policy_stats_saturated",
        "block_aligner_execution_stats",
        "prefilter_execution_stats",
    });
static_assert(k_simd_execution_stats_keys.size() == 32);

inline constexpr auto k_simd_execution_stats_revision6_keys =
    std::to_array<std::string_view>({
        "schema", "revision", "requested_minimizer_algorithm",
        "observed_minimizer_algorithm", "requested_chaining_algorithm",
        "observed_chaining_algorithm", "requested_base_alignment_algorithm",
        "observed_base_alignment_algorithm", "requested_policy", "observed_policy",
        "requested_alignment_algorithm", "observed_alignment_algorithm",
        "downstream_active_wall_available", "downstream_active_wall_ns",
        "downstream_active_wall_invocation_count",
        "downstream_active_wall_unavailable_invocation_count",
        "downstream_work_ns", "downstream_work_saturated",
        "mapq_execution_available", "mapq_executed", "mapq_execution_conflict",
        "benchmark_e2e_interval_union", "nthash_vector_batches",
        "nthash_resident_vector_stream_kernel_calls",
        "simd_chaining_exact_policy_calls",
        "simd_chaining_bounded_policy_calls",
        "simd_chaining_bounded_score_batches",
        "simd_chaining_bounded_cutoff_stops",
        "simd_chaining_bounded_candidates_skipped",
        "simd_chaining_policy_rejections",
        "are_simd_chaining_policy_stats_saturated",
        "block_aligner_execution_stats", "prefilter_execution_stats",
    });
static_assert(k_simd_execution_stats_revision6_keys.size() == 33);

// Keep the existing requested/observed fields as the sole algorithm identities.
// This is compile-time schema validation, not an execution metric or a hot-path
// check; the legacy and current payload revisions retain their exact layouts.
consteval bool simd_execution_stats_keys_obey_identity_contract(
    std::span<const std::string_view> keys) {
    for (std::size_t i = 0; i < keys.size(); ++i) {
        if (keys[i] == "selected_seed_engine" ||
            keys[i] == "actual_seed_engine" ||
            keys[i] == "selected_chain_engine" ||
            keys[i] == "actual_chain_engine") {
            return false;
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (keys[i] == keys[j]) return false;
        }
    }
    return true;
}
static_assert(simd_execution_stats_keys_obey_identity_contract(
    k_simd_execution_stats_keys));
static_assert(simd_execution_stats_keys_obey_identity_contract(
    k_simd_execution_stats_revision6_keys));

inline constexpr auto k_benchmark_e2e_interval_union_keys =
    std::to_array<std::string_view>({
        "available", "union_ns", "interval_count", "cigar_interval_count",
        "mapq_interval_count", "finalization_count",
        "physical_interval_semantics", "operation_origin_scope",
        "derivation_contract", "failure_code", "origins",
    });
static_assert(k_benchmark_e2e_interval_union_keys.size() == 11);

inline constexpr auto k_benchmark_e2e_interval_origin_keys =
    std::to_array<std::string_view>({
        "logical_target_row", "query_ordinal", "kind", "algorithm",
        "inclusive_work", "interval_count",
    });
static_assert(k_benchmark_e2e_interval_origin_keys.size() == 6);

inline constexpr auto k_block_aligner_execution_stats_keys =
    std::to_array<std::string_view>({
        "available", "consistent", "executed_calls", "nonempty_calls",
        "nonempty_calls_with_simd", "simd_dp_vector_batches",
        "simd_dp_cells", "forbidden_ksw2_calls",
        "forbidden_scalar_biological_fallbacks", "saturated",
    });
static_assert(k_block_aligner_execution_stats_keys.size() == 10);

inline constexpr auto k_prefilter_execution_stats_keys = std::to_array<std::string_view>({
    "available",
    "dispatches",
    "sneaky_snake_dispatches",
    "gatekeeper_dispatches",
    "scalar_dispatches",
    "simd_dispatches",
    "vulkan_dispatches",
    "jobs",
    "evaluated_jobs",
    "length_64_jobs",
    "length_difference_jobs",
    "unsupported_jobs",
    "rejected_jobs",
    "rejected_regions",
    "surviving_regions",
    "mask_rejection_candidates",
    "exact_guard_activations",
    "guard_confirmed_rejections",
    "hamming_exact_rejections",
    "saturated",
});
static_assert(k_prefilter_execution_stats_keys.size() == 20);

inline constexpr auto k_persisted_gpu_operation_stats_keys = std::to_array<std::string_view>({
    "schema",
    "revision",
    "execution_id",
    "backend_pid",
    "database_oid",
    "is_completed",
    "is_partial",
    "is_fully_drained",
    "is_consistent",
    "is_complete",
    "is_saturated",
    "dropped_operation_count",
    "operation_count",
    "accepted_operation_count",
    "complete_operation_count",
    "job_count",
    "accepted_job_count",
    "cache_validation",
    "operations",
});
static_assert(k_persisted_gpu_operation_stats_keys.size() == 19);

inline constexpr auto k_persisted_gpu_operation_keys = std::to_array<std::string_view>({
    "operation_identity",
    "expected_jobs",
    "validated_jobs",
    "consumed_jobs",
    "accepted_jobs",
    "burned_jobs",
    "pending_jobs",
    "staged_jobs",
    "published_jobs",
    "publication_commits",
    "query_pipeline_receipt_count",
    "shape_pipeline_receipt_count",
    "alignment_pipeline_receipt_count",
    "alignment_submission_count",
    "mapq_pipeline_receipt_count",
    "mapq_submission_count",
    "mapq_result_count",
    "mapq_published_count",
    "mapq_result_digest",
    "submission_count",
    "query_pipeline_operation_identity",
    "shape_pipeline_operation_identity",
    "query_submission_ordinals",
    "seed_receipt_identity",
    "cache_token_identity",
    "cache_validation_identity",
    "generation_id",
    "source_identity",
    "cold_load_identity",
    "source",
    "environment",
    "cache_metrics",
    "jobs",
});
static_assert(k_persisted_gpu_operation_keys.size() == 33);

inline constexpr auto k_persisted_gpu_cache_validation_keys = std::to_array<std::string_view>({
    "revision",
    "identity",
    "is_consistent",
    "is_saturated",
    "archive_count",
    "archive_bytes",
    "archive_checksum",
    "receipt_count",
    "hash_passes",
    "hashed_bytes",
    "private_files_created",
    "private_files_unlinked",
    "live_private_paths",
    "pipeline_filesystem_hash_passes",
    "pipeline_filesystem_hashed_bytes",
    "helper_protocol_revision",
    "helper_build_identity",
    "helper_source",
    "helper_executable_size",
    "helper_executable_sha256",
    "helper_metal_registry_id",
    "device_uuid",
    "api_version",
    "driver_version",
    "vendor_id",
    "device_id",
    "pipeline_cache_uuid",
    "seed_receipt_identity",
    "seed_sha256",
    "seed_size",
    "manifest_checksum",
});
static_assert(k_persisted_gpu_cache_validation_keys.size() == 31);

inline constexpr auto k_persisted_gpu_environment_keys =
    std::to_array<std::string_view>({
        "device_uuid", "api_version", "driver_version", "vendor_id",
        "device_id", "pipeline_cache_uuid", "seed_sha256", "seed_size",
        "manifest_checksum",
    });
static_assert(k_persisted_gpu_environment_keys.size() == 9);

inline constexpr auto k_persisted_gpu_cache_metrics_keys =
    std::to_array<std::string_view>({
        "pipeline_cache_loads", "pipeline_cache_seed_bytes",
        "pipeline_cache_load_ns", "pipeline_creates",
        "pipeline_creation_ns",
    });
static_assert(k_persisted_gpu_cache_metrics_keys.size() == 5);

inline constexpr auto k_persisted_gpu_job_keys =
    std::to_array<std::string_view>({
        "logical_target_row", "logical_query_ordinal", "query_index",
        "query_length", "query_minimizer_count", "query_tile_count",
        "query", "direct", "submissions", "status", "forbidden",
        "coordinate",
    });
static_assert(k_persisted_gpu_job_keys.size() == 12);

inline constexpr auto k_persisted_gpu_query_keys =
    std::to_array<std::string_view>({
        "count_dispatches", "scan_dispatches", "emit_dispatches",
        "completed_roundtrips",
    });
static_assert(k_persisted_gpu_query_keys.size() == 4);

inline constexpr auto k_persisted_gpu_direct_keys =
    std::to_array<std::string_view>({
        "count_dispatches", "scan_dispatches", "anchor_count",
        "emit_dispatches", "sort_operations", "range_build_dispatches",
        "range_count", "chain_dispatches", "chain_backtrack_dispatches",
        "coordinate_dispatches",
    });
static_assert(k_persisted_gpu_direct_keys.size() == 10);

inline constexpr auto k_persisted_gpu_submission_keys =
    std::to_array<std::string_view>({
        "stage", "operation_identity", "ordinal", "submit_wait_identity",
    });
static_assert(k_persisted_gpu_submission_keys.size() == 4);

inline constexpr auto k_persisted_gpu_status_keys =
    std::to_array<std::string_view>({
        "count_checks", "emit_checks", "status_or", "result_readbacks",
    });
static_assert(k_persisted_gpu_status_keys.size() == 4);

inline constexpr auto k_persisted_gpu_forbidden_keys =
    std::to_array<std::string_view>({
        "coordinate_vote_dispatches", "cpu_or_simd_biological_calls",
        "recompute_calls", "fallback_calls",
    });
static_assert(k_persisted_gpu_forbidden_keys.size() == 4);

inline constexpr auto k_persisted_gpu_coordinate_keys =
    std::to_array<std::string_view>({
        "valid", "query_start", "query_end", "target_start", "target_end",
        "match_length", "block_length", "chain_score", "seed_count",
        "is_reverse",
    });
static_assert(k_persisted_gpu_coordinate_keys.size() == 10);

template<std::size_t p_key_count>
[[nodiscard]] bool json_has_exact_keys(
        const nlohmann::json& value,
        const std::array<std::string_view, p_key_count>& keys) {
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
        abort_pipeline_stats_capture(
            "alignment stats JSON counter is absent or ill-typed");
    }
    if (found->is_number_unsigned()) {
        const auto value = found->get<std::uint64_t>();
        if (value > static_cast<std::uint64_t>(
                (std::numeric_limits<std::int64_t>::max)())) {
            abort_pipeline_stats_capture(
                "alignment stats JSON counter exceeds SQL bigint");
        }
        return static_cast<std::int64_t>(value);
    }
    const auto value = found->get<std::int64_t>();
    if (value < 0) {
        abort_pipeline_stats_capture(
            "alignment stats JSON counter is negative");
    }
    return value;
}

/** Render a bounded JSON-style diagnostic excerpt without emitting raw controls. */
[[nodiscard]] std::string escaped_json_capture_excerpt(
        std::string_view text,
        const std::size_t maximum_input_bytes) {
    constexpr char hexadecimal[] = "0123456789abcdef";
    const std::size_t input_bytes = std::min(text.size(), maximum_input_bytes);
    std::string escaped;
    escaped.reserve(input_bytes * 6u + (text.size() > input_bytes ? 3u : 0u));
    for (std::size_t index = 0u; index < input_bytes; ++index) {
        const unsigned char byte = static_cast<unsigned char>(text[index]);
        switch (byte) {
            case '\\': escaped += "\\\\"; break;
            case '"': escaped += "\\\""; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default:
                if (byte >= 0x20u && byte <= 0x7eu) {
                    escaped.push_back(static_cast<char>(byte));
                } else {
                    escaped += "\\u00";
                    escaped.push_back(hexadecimal[byte >> 4u]);
                    escaped.push_back(hexadecimal[byte & 0x0fu]);
                }
                break;
        }
    }
    if (text.size() > input_bytes) escaped += "...";
    return escaped;
}

[[nodiscard]] nlohmann::json parse_exact_json_cell(
        const std::optional<std::string>& cell,
        std::string_view diagnostic) {
    if (!cell.has_value()) {
        abort_pipeline_stats_capture(fmt::format("{}: SQL NULL JSON cell", diagnostic));
    }
    try {
        return nlohmann::json::parse(*cell);
    } catch (const nlohmann::json::exception& error) {
        constexpr std::size_t maximum_cell_excerpt_bytes = 256u;
        constexpr std::size_t maximum_exception_excerpt_bytes = 160u;
        const char* const exception_text = error.what();
        abort_pipeline_stats_capture(fmt::format(
            "{}: JSON parse failure (cell_bytes={}, cell_excerpt=\"{}\", exception=\"{}\")",
            diagnostic,
            cell->size(),
            escaped_json_capture_excerpt(*cell, maximum_cell_excerpt_bytes),
            escaped_json_capture_excerpt(
                exception_text == nullptr ? std::string_view{} : std::string_view{exception_text},
                maximum_exception_excerpt_bytes)));
    }
}

/// Read one required JSON Boolean or fail the stats capture closed.
[[nodiscard]] bool required_json_boolean(
        const nlohmann::json& object,
        std::string_view key) {
    const auto found = object.find(std::string{key});
    if (found == object.end() || !found->is_boolean()) {
        abort_pipeline_stats_capture(
            "persisted GPU stats Boolean is absent or ill-typed");
    }
    return found->get<bool>();
}

/// Read one required JSON string without copying its retained value.
[[nodiscard]] const std::string& required_json_text(
        const nlohmann::json& object,
        std::string_view key) {
    const auto found = object.find(std::string{key});
    if (found == object.end() || !found->is_string()) {
        abort_pipeline_stats_capture(
            "persisted GPU stats text is absent or ill-typed");
    }
    return found->get_ref<const std::string&>();
}

/// Read one required JSON array from the retained stats object.
[[nodiscard]] const nlohmann::json& required_json_array(
        const nlohmann::json& object,
        std::string_view key) {
    const auto found = object.find(std::string{key});
    if (found == object.end() || !found->is_array()) {
        abort_pipeline_stats_capture(
            "persisted GPU stats array is absent or ill-typed");
    }
    return *found;
}

/// Read one required nested JSON object from the retained stats object.
[[nodiscard]] const nlohmann::json& required_json_object(
        const nlohmann::json& object,
        std::string_view key) {
    const auto found = object.find(std::string{key});
    if (found == object.end() || !found->is_object()) {
        abort_pipeline_stats_capture(
            "persisted GPU stats object is absent or ill-typed");
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
        abort_pipeline_stats_capture(
            "persisted GPU stats identity is zero or malformed");
    }
    return value;
}

/// Read one nonnegative SQL-bigint-compatible JSON array element.
[[nodiscard]] std::int64_t required_json_integer_value(
        const nlohmann::json& value) {
    if (!(value.is_number_unsigned() || value.is_number_integer())) {
        abort_pipeline_stats_capture(
            "persisted GPU stats array integer is ill-typed");
    }
    if (value.is_number_unsigned()) {
        const auto parsed = value.get<std::uint64_t>();
        if (parsed > static_cast<std::uint64_t>(
                (std::numeric_limits<std::int64_t>::max)())) {
            abort_pipeline_stats_capture(
                "persisted GPU stats array integer exceeds SQL bigint");
        }
        return static_cast<std::int64_t>(parsed);
    }
    const auto parsed = value.get<std::int64_t>();
    if (parsed < 0) {
        abort_pipeline_stats_capture(
            "persisted GPU stats array integer is negative");
    }
    return parsed;
}

void capture_alignment_stats_json(
        const it::rowset_t& rows,
        e2e_output& out) {
    const nlohmann::json cold = parse_exact_json_cell(
        rows.front()[149], "cold-index stats JSON failed to parse");
    const bool cold_header_valid = json_has_exact_keys(
            cold, k_cold_index_stats_keys)
        && cold["schema"].is_string()
        && cold["schema"] == "bioseq.cold_index_preparation"
        && cold["revision"].is_number_integer()
        && cold["revision"] == 1
        && cold["timing_semantics"].is_string()
        && cold["timing_semantics"] == "payload_inclusive_wall"
        && cold["decode_timing_available"].is_boolean()
        && !cold["decode_timing_available"].get<bool>()
        && cold["transfer_timing_available"].is_boolean()
        && !cold["transfer_timing_available"].get<bool>()
        && cold["decode_ns"].is_null()
        && cold["transfer_ns"].is_null()
        && cold["are_stats_saturated"].is_boolean();
    if (!cold_header_valid) {
        abort_pipeline_stats_capture(
            "cold-index stats JSON has the wrong exact schema or types");
    }
    for (const auto key : k_cold_index_stats_keys) {
        if (key == "schema" || key == "revision"
            || key == "timing_semantics"
            || key == "decode_timing_available"
            || key == "transfer_timing_available"
            || key == "decode_ns" || key == "transfer_ns"
            || key == "are_stats_saturated") {
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
        cold["are_stats_saturated"].get<bool>() ? 1 : 0;

    const nlohmann::json simd = parse_exact_json_cell(
        rows.front()[150], "SIMD execution stats JSON failed to parse");
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
    const auto valid_minimizer_algorithm_name = [](const nlohmann::json& value) {
        if (!value.is_string()) return false;
        const std::string& algorithm = value.get_ref<const std::string&>();
        return algorithm == "none" || algorithm == "scalar" ||
               algorithm == "simd_minimap2_reference" ||
               algorithm == "two_stack_nthash" ||
               algorithm == "vulkan_minimap_integer" ||
               algorithm == "bounded_nthash_simd" || algorithm == "mixed_invalid";
    };
    const auto valid_chaining_algorithm_name = [](const nlohmann::json& value) {
        if (!value.is_string()) return false;
        const std::string& algorithm = value.get_ref<const std::string&>();
        return algorithm == "none" || algorithm == "scalar_minimap2" ||
               algorithm == "simd_minimap2" || algorithm == "range_minimum_query" ||
               algorithm == "vulkan_tiled_dynamic_programming" ||
               algorithm == "mixed_invalid";
    };
    const auto valid_base_alignment_algorithm_name = [](const nlohmann::json& value) {
        if (!value.is_string()) return false;
        const std::string& algorithm = value.get_ref<const std::string&>();
        return algorithm == "none" || algorithm == "ksw2" ||
               algorithm == "block_aligner" ||
               algorithm == "vulkan_simple_cigar" ||
               algorithm == "mixed_invalid";
    };
    const auto nonnegative_integer_or_null = [](const nlohmann::json& value) {
        return value.is_null() || value.is_number_unsigned() || value.is_number_integer();
    };
    const bool simd_revision5 = simd.contains("revision")
        && simd["revision"].is_number_integer()
        && simd["revision"] == 5;
    const bool simd_revision6 = simd.contains("revision")
        && simd["revision"].is_number_integer()
        && simd["revision"] == 6;
    const bool simd_header_valid =
        ((simd_revision5 && json_has_exact_keys(
                simd, k_simd_execution_stats_keys))
         || (simd_revision6 && json_has_exact_keys(
                simd, k_simd_execution_stats_revision6_keys)))
        && simd["schema"].is_string()
        && simd["schema"] == "bioseq.simd_execution_stats"
        && valid_minimizer_algorithm_name(simd["requested_minimizer_algorithm"])
        && valid_minimizer_algorithm_name(simd["observed_minimizer_algorithm"])
        && valid_chaining_algorithm_name(simd["requested_chaining_algorithm"])
        && valid_chaining_algorithm_name(simd["observed_chaining_algorithm"])
        && valid_base_alignment_algorithm_name(
            simd["requested_base_alignment_algorithm"])
        && valid_base_alignment_algorithm_name(
            simd["observed_base_alignment_algorithm"])
        && valid_policy_name(simd["requested_policy"])
        && valid_policy_name(simd["observed_policy"])
        && valid_alignment_algorithm_name(
            simd["requested_alignment_algorithm"])
        && valid_alignment_algorithm_name(
            simd["observed_alignment_algorithm"])
        && simd["downstream_active_wall_available"].is_boolean()
        && nonnegative_integer_or_null(simd["downstream_active_wall_ns"])
        && simd["downstream_work_saturated"].is_boolean()
        && simd["mapq_execution_available"].is_boolean()
        && simd["mapq_executed"].is_boolean()
        && simd["mapq_execution_conflict"].is_boolean()
        && simd["are_simd_chaining_policy_stats_saturated"].is_boolean()
        && json_has_exact_keys(
            simd["block_aligner_execution_stats"],
            k_block_aligner_execution_stats_keys)
        && simd["block_aligner_execution_stats"]["available"].is_boolean()
        && simd["block_aligner_execution_stats"]["consistent"].is_boolean()
        && simd["block_aligner_execution_stats"]["saturated"].is_boolean()
        && json_has_exact_keys(simd["prefilter_execution_stats"],
                               k_prefilter_execution_stats_keys)
        && simd["prefilter_execution_stats"]["available"].is_boolean()
        && simd["prefilter_execution_stats"]["saturated"].is_boolean();
    if (!simd_header_valid) {
        abort_pipeline_stats_capture(
            "SIMD execution stats JSON has the wrong exact schema or types");
    }
    const nlohmann::json& block =
        simd["block_aligner_execution_stats"];
    const nlohmann::json& prefilter = simd["prefilter_execution_stats"];
    out.simd_stats_schema_valid = 1;
    out.benchmark_e2e_interval_schema_revision = simd_revision6 ? 6 : 5;
    out.attested_requested_minimizer_algorithm =
        simd["requested_minimizer_algorithm"].get<std::string>();
    out.observed_minimizer_algorithm =
        simd["observed_minimizer_algorithm"].get<std::string>();
    out.attested_requested_chaining_algorithm =
        simd["requested_chaining_algorithm"].get<std::string>();
    out.observed_chaining_algorithm =
        simd["observed_chaining_algorithm"].get<std::string>();
    out.attested_requested_base_alignment_algorithm =
        simd["requested_base_alignment_algorithm"].get<std::string>();
    out.observed_base_alignment_algorithm =
        simd["observed_base_alignment_algorithm"].get<std::string>();
    out.requested_simd_chaining_policy =
        simd["requested_policy"].get<std::string>();
    out.observed_simd_chaining_policy =
        simd["observed_policy"].get<std::string>();
    out.requested_simd_alignment_algorithm =
        simd["requested_alignment_algorithm"].get<std::string>();
    out.observed_simd_alignment_algorithm =
        simd["observed_alignment_algorithm"].get<std::string>();
    out.downstream_active_wall_available =
        simd["downstream_active_wall_available"].get<bool>() ? 1 : 0;
    out.downstream_active_wall_invocation_count = required_json_counter(
        simd, "downstream_active_wall_invocation_count");
    out.downstream_active_wall_unavailable_invocation_count = required_json_counter(
        simd, "downstream_active_wall_unavailable_invocation_count");
    out.downstream_work_ns = required_json_counter(simd, "downstream_work_ns");
    out.downstream_work_saturated =
        simd["downstream_work_saturated"].get<bool>() ? 1 : 0;
    out.mapq_execution_available =
        simd["mapq_execution_available"].get<bool>() ? 1 : 0;
    out.mapq_executed = simd["mapq_executed"].get<bool>() ? 1 : 0;
    out.mapq_execution_conflict =
        simd["mapq_execution_conflict"].get<bool>() ? 1 : 0;
    const bool active_wall_shape_valid =
        out.downstream_active_wall_unavailable_invocation_count <=
            out.downstream_active_wall_invocation_count &&
        out.downstream_active_wall_available ==
            (out.downstream_active_wall_invocation_count == 1 &&
                     out.downstream_active_wall_unavailable_invocation_count == 0
                 ? 1
                 : 0) &&
        (out.downstream_active_wall_available == 1
             ? !simd["downstream_active_wall_ns"].is_null()
             : simd["downstream_active_wall_ns"].is_null());
    if (!active_wall_shape_valid) {
        abort_pipeline_stats_capture(
            "downstream active-wall receipt has an inconsistent availability shape");
    }
    out.downstream_active_wall_ns = out.downstream_active_wall_available == 1
        ? required_json_counter(simd, "downstream_active_wall_ns")
        : -1;
    const bool mapq_shape_valid = out.mapq_execution_available == 1 ||
        out.mapq_executed == 0 || out.mapq_execution_conflict == 1;
    if (!mapq_shape_valid) {
        abort_pipeline_stats_capture(
            "MAPQ execution receipt has an inconsistent availability shape");
    }
    if (simd_revision6) {
        const nlohmann::json& interval =
            simd["benchmark_e2e_interval_union"];
        if (!json_has_exact_keys(
                interval, k_benchmark_e2e_interval_union_keys) ||
            !interval["available"].is_boolean() ||
            !nonnegative_integer_or_null(interval["union_ns"]) ||
            !interval["physical_interval_semantics"].is_string() ||
            interval["physical_interval_semantics"] !=
                "half_open_monotonic_same_statement_union" ||
            !interval["operation_origin_scope"].is_string() ||
            interval["operation_origin_scope"] !=
                "alignment_stats_execution_id_logical_target_row_query_ordinal" ||
            !interval["derivation_contract"].is_string() ||
            interval["derivation_contract"] !=
                "runner_subtracts_union_from_same_repeat_outer_e2e_before_aggregation" ||
            !interval["failure_code"].is_string() ||
            !interval["origins"].is_array()) {
            abort_pipeline_stats_capture(
                "benchmark E2E interval union has the wrong exact schema or contract");
        }
        out.benchmark_e2e_interval_available =
            interval["available"].get<bool>() ? 1 : 0;
        out.benchmark_e2e_interval_count =
            required_json_counter(interval, "interval_count");
        out.benchmark_e2e_cigar_interval_count =
            required_json_counter(interval, "cigar_interval_count");
        out.benchmark_e2e_mapq_interval_count =
            required_json_counter(interval, "mapq_interval_count");
        out.benchmark_e2e_interval_finalization_count =
            required_json_counter(interval, "finalization_count");
        out.benchmark_e2e_interval_failure_code =
            interval["failure_code"].get<std::string>();
		const bool failure_code_valid =
			out.benchmark_e2e_interval_failure_code == "none" ||
			out.benchmark_e2e_interval_failure_code == "finalization_count" ||
			out.benchmark_e2e_interval_failure_code == "capacity_overflow" ||
			out.benchmark_e2e_interval_failure_code == "tracker_invalid" ||
            out.benchmark_e2e_interval_failure_code ==
                "origin_or_identity_mismatch" ||
            out.benchmark_e2e_interval_failure_code == "incomplete";
        if (!failure_code_valid) {
            abort_pipeline_stats_capture(
                "benchmark E2E interval union has an unknown failure code");
        }
        out.benchmark_e2e_interval_union_ns =
            out.benchmark_e2e_interval_available == 1
                ? required_json_counter(interval, "union_ns")
                : -1;
        out.benchmark_e2e_interval_origins = interval["origins"];

        const bool interval_count_shape =
            out.benchmark_e2e_cigar_interval_count <=
                out.benchmark_e2e_interval_count &&
            out.benchmark_e2e_mapq_interval_count ==
                out.benchmark_e2e_interval_count -
                    out.benchmark_e2e_cigar_interval_count;
        bool origins_valid = !out.benchmark_e2e_interval_origins.empty();
        bool observed_cigar_origin = false;
        bool observed_mapq_origin = false;
        std::optional<std::tuple<std::int64_t, std::int64_t, int, int>>
            previous_origin;
        for (const nlohmann::json& origin :
             out.benchmark_e2e_interval_origins) {
            if (!json_has_exact_keys(
                    origin, k_benchmark_e2e_interval_origin_keys) ||
                !origin["kind"].is_string() ||
                !origin["algorithm"].is_string() ||
                !origin["inclusive_work"].is_string()) {
                origins_valid = false;
                break;
            }
            const std::int64_t logical_target_row =
                required_json_counter(origin, "logical_target_row");
            const std::int64_t query_ordinal =
                required_json_counter(origin, "query_ordinal");
            const std::int64_t interval_count =
                required_json_counter(origin, "interval_count");
            const std::string kind = origin["kind"].get<std::string>();
            const std::string algorithm =
                origin["algorithm"].get<std::string>();
            const std::string inclusive_work =
                origin["inclusive_work"].get<std::string>();
            const int kind_rank = kind == "cigar" ? 0
                : kind == "mapq" ? 1 : -1;
            const int algorithm_rank = algorithm == "ksw2" ? 0
                : algorithm == "block_aligner" ? 1
                : algorithm == "vulkan_simple_cigar" ? 2
                : algorithm == "cpu_mapping_quality" ? 3
                : algorithm == "vulkan_mapping_quality" ? 4 : -1;
            const bool cigar_origin = kind_rank == 0 &&
                ((algorithm == "ksw2" && inclusive_work ==
                    "alignment_dp_trace_storage_traceback_cigar_assembly_result_materialization_inclusive") ||
                 (algorithm == "block_aligner" && inclusive_work ==
                    "block_dp_trace_storage_traceback_cigar_assembly_result_materialization_inclusive") ||
                 (algorithm == "vulkan_simple_cigar" && inclusive_work ==
                    "host_packing_upload_gpu_alignment_traceback_wait_readback_decode_result_materialization_inclusive")) &&
                algorithm == out.observed_base_alignment_algorithm;
            const bool cpu_mapq_backend =
                out.actual_backend == "scalar_cpu"
                || out.actual_backend == "simd_cpu"
                || out.actual_backend == "mixed_cpu"
                || out.actual_backend == "hybrid_cpu_vulkan";
            const bool cpu_mapq_origin = kind_rank == 1
                && algorithm == "cpu_mapping_quality"
                && inclusive_work == "compute_mapping_quality_only"
                && cpu_mapq_backend;
            const bool vulkan_mapq_origin = kind_rank == 1
                && algorithm == "vulkan_mapping_quality"
                && inclusive_work ==
                    "host_validation_upload_gpu_classification_mapq_wait_"
                    "readback_publication_inclusive"
                && out.actual_backend == "vulkan_gpu";
            const bool mapq_origin = cpu_mapq_origin || vulkan_mapq_origin;
            const auto identity = std::tuple{
                logical_target_row, query_ordinal, kind_rank, algorithm_rank};
            if (interval_count <= 0 ||
                kind_rank < 0 || algorithm_rank < 0 ||
                (!cigar_origin && !mapq_origin) ||
                (previous_origin.has_value() && identity <= *previous_origin)) {
                origins_valid = false;
                break;
            }
            previous_origin = identity;
            observed_cigar_origin = observed_cigar_origin || cigar_origin;
            observed_mapq_origin = observed_mapq_origin || mapq_origin;
        }
        const bool origin_phase_shape = origins_valid &&
            observed_cigar_origin ==
                (out.benchmark_e2e_cigar_interval_count != 0) &&
            observed_mapq_origin ==
                (out.benchmark_e2e_mapq_interval_count != 0);
        const bool available_shape =
            out.benchmark_e2e_interval_available == 1
                ? !interval["union_ns"].is_null() &&
                    out.benchmark_e2e_interval_union_ns >= 0 &&
                    out.benchmark_e2e_interval_count > 0 &&
                    out.benchmark_e2e_interval_finalization_count == 1 &&
                    out.benchmark_e2e_interval_failure_code == "none" &&
                    interval_count_shape && origin_phase_shape
                : interval["union_ns"].is_null() &&
                    out.benchmark_e2e_interval_failure_code != "none" &&
                    ((out.benchmark_e2e_interval_failure_code ==
                          "finalization_count") ==
                     (out.benchmark_e2e_interval_finalization_count != 1));
        if (!available_shape) {
            abort_pipeline_stats_capture(
                "benchmark E2E interval union availability/origin shape is inconsistent");
        }
    }
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
    out.simd_chaining_policy_stats_saturated =
        simd["are_simd_chaining_policy_stats_saturated"].get<bool>() ? 1 : 0;
    out.block_aligner_execution_stats_available =
        block["available"].get<bool>() ? 1 : 0;
    out.block_aligner_execution_stats_consistent =
        block["consistent"].get<bool>() ? 1 : 0;
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
    out.block_aligner_execution_stats_saturated =
        block["saturated"].get<bool>() ? 1 : 0;
    out.prefilter_execution_stats = prefilter;
    out.prefilter_execution_stats_available =
        prefilter["available"].get<bool>() ? 1 : 0;
    out.prefilter_execution_stats_saturated =
        prefilter["saturated"].get<bool>() ? 1 : 0;
    __int128 prefilter_activity = 0;
    for (const std::string_view key : k_prefilter_execution_stats_keys) {
        if (key == "available" || key == "saturated") continue;
        prefilter_activity += required_json_counter(prefilter, key);
    }
    out.prefilter_activity_count = prefilter_activity >
            static_cast<__int128>((std::numeric_limits<std::int64_t>::max)())
        ? (std::numeric_limits<std::int64_t>::max)()
        : static_cast<std::int64_t>(prefilter_activity);
}

void capture_minimizer_fragment_reader_stats(
        const it::connection_test_context& connection,
        e2e_output& out) {
    const it::row_t row = it::query_one(
        connection,
        "SELECT bioseq."
        "last_alignment_minimizer_fragment_reader_stats();");
    if (row.size() != 1) {
        abort_pipeline_stats_capture(
            "minimizer-fragment reader stats have an invalid scalar shape");
    }
    const nlohmann::json stats = parse_exact_json_cell(
        row.front(), "minimizer-fragment reader stats JSON failed to parse");
    const bool header_valid = json_has_exact_keys(
            stats, k_minimizer_fragment_reader_stats_keys)
        && stats["schema"].is_string()
        && stats["schema"] == "bioseq.grouped_soa_reader_memory_stats"
        && stats["revision"].is_number_integer()
        && stats["revision"] == 6
        && stats["are_fragment_native_stats_saturated"].is_boolean()
        && stats["are_grouped_soa_reader_memory_stats_saturated"].is_boolean()
        && stats["are_stats_saturated"].is_boolean();
    if (!header_valid) {
        abort_pipeline_stats_capture(
            "minimizer-fragment reader stats have the wrong exact schema or types");
    }
    for (const std::string_view key : k_minimizer_fragment_reader_stats_keys) {
        if (key == "schema"
            || key == "are_fragment_native_stats_saturated"
            || key == "are_grouped_soa_reader_memory_stats_saturated"
            || key == "are_stats_saturated") {
            continue;
        }
        (void)required_json_counter(stats, key);
    }

    const std::int64_t execution_id =
        required_json_counter(stats, "execution_id");
    const std::int64_t backend_pid =
        required_json_counter(stats, "backend_pid");
    const std::int64_t database_oid =
        required_json_counter(stats, "database_oid");
    if (execution_id != out.alignment_execution_id
        || backend_pid != out.alignment_backend_pid
        || database_oid != out.alignment_database_oid
        || stats["are_stats_saturated"].get<bool>()
            != (out.stats_saturated != 0)) {
        abort_pipeline_stats_capture(
            "minimizer-fragment reader stats identity/saturation mismatch");
    }

    out.grouped_soa_reader_capacity_accounting_passes = required_json_counter(
        stats, "grouped_soa_reader_capacity_accounting_passes");
    out.grouped_soa_reader_global_position_storage_preparations =
        required_json_counter(
            stats,
            "grouped_soa_reader_global_position_storage_preparations");
    out.grouped_soa_reader_global_position_storage_allocations =
        required_json_counter(
            stats,
            "grouped_soa_reader_global_position_storage_allocations");
    out.grouped_soa_reader_position_append_growth_reservations =
        required_json_counter(
            stats,
            "grouped_soa_reader_position_append_growth_reservations");
    out.grouped_soa_reader_retained_position_count = required_json_counter(
        stats, "grouped_soa_reader_retained_position_count");
    out.grouped_soa_reader_retained_position_capacity = required_json_counter(
        stats, "grouped_soa_reader_retained_position_capacity");
    out.grouped_soa_reader_directory_capacity_bytes = required_json_counter(
        stats, "grouped_soa_reader_directory_capacity_bytes");
    out.grouped_soa_reader_query_hash_capacity_bytes = required_json_counter(
        stats, "grouped_soa_reader_query_hash_capacity_bytes");
    out.grouped_soa_reader_query_record_capacity_bytes = required_json_counter(
        stats, "grouped_soa_reader_query_record_capacity_bytes");
    out.grouped_soa_reader_ordinal_route_capacity_bytes = required_json_counter(
        stats, "grouped_soa_reader_ordinal_route_capacity_bytes");
    out.grouped_soa_reader_selected_extent_capacity_bytes = required_json_counter(
        stats, "grouped_soa_reader_selected_extent_capacity_bytes");
    out.grouped_soa_reader_current_selected_control_capacity_bytes =
        required_json_counter(
            stats,
            "grouped_soa_reader_current_selected_control_capacity_bytes");
    out.grouped_soa_reader_global_position_capacity_bytes = required_json_counter(
        stats, "grouped_soa_reader_global_position_capacity_bytes");
    out.grouped_soa_reader_control_payload_capacity_bytes = required_json_counter(
        stats, "grouped_soa_reader_control_payload_capacity_bytes");
    out.grouped_soa_reader_owned_capacity_bytes = required_json_counter(
        stats, "grouped_soa_reader_owned_capacity_bytes");
    out.grouped_soa_reader_admitted_bytes = required_json_counter(
        stats, "grouped_soa_reader_admitted_bytes");
    out.grouped_soa_reader_exact_position_image_checks = required_json_counter(
        stats, "grouped_soa_reader_exact_position_image_checks");
    out.grouped_soa_reader_exact_position_image_failures = required_json_counter(
        stats, "grouped_soa_reader_exact_position_image_failures");
    out.grouped_soa_reader_retained_layout_checks = required_json_counter(
        stats, "grouped_soa_reader_retained_layout_checks");
    out.grouped_soa_reader_retained_layout_failures = required_json_counter(
        stats, "grouped_soa_reader_retained_layout_failures");
    out.fragment_native_waves = required_json_counter(
        stats, "fragment_native_waves");
    out.fragment_native_queries = required_json_counter(
        stats, "fragment_native_queries");
    out.fragment_native_occurrence_fill_ns = required_json_counter(
        stats, "fragment_native_occurrence_fill_ns");
    out.fragment_native_occurrence_radix_ns = required_json_counter(
        stats, "fragment_native_occurrence_radix_ns");
    out.fragment_native_unique_plan_build_ns = required_json_counter(
        stats, "fragment_native_unique_plan_build_ns");
    out.fragment_native_query_plan_radix_fill_ns = required_json_counter(
        stats, "fragment_native_query_plan_radix_fill_ns");
    out.fragment_native_control_count_pass_ns = required_json_counter(
        stats, "fragment_native_control_count_pass_ns");
    out.fragment_native_rejected_control_validation_ns =
        required_json_counter(
            stats, "fragment_native_rejected_control_validation_ns");
    out.fragment_native_orientation_layout_ns = required_json_counter(
        stats, "fragment_native_orientation_layout_ns");
    out.fragment_native_direct_emission_ns = required_json_counter(
        stats, "fragment_native_direct_emission_ns");
    out.fragment_native_local_sort_ns = required_json_counter(
        stats, "fragment_native_local_sort_ns");
    out.fragment_native_tie_normalization_ns = required_json_counter(
        stats, "fragment_native_tie_normalization_ns");
    out.fragment_native_control_count_bytes = required_json_counter(
        stats, "fragment_native_control_count_bytes");
    out.fragment_native_rejected_control_validation_bytes =
        required_json_counter(
            stats, "fragment_native_rejected_control_validation_bytes");
    out.fragment_native_pass2_position_bytes = required_json_counter(
        stats, "fragment_native_pass2_position_bytes");
    out.fragment_native_pass3_position_bytes = required_json_counter(
        stats, "fragment_native_pass3_position_bytes");
    out.fragment_native_anchors_constructed = required_json_counter(
        stats, "fragment_native_anchors_constructed");
    out.fragment_native_local_sort_bytes = required_json_counter(
        stats, "fragment_native_local_sort_bytes");
    out.fragment_native_tie_normalization_bytes = required_json_counter(
        stats, "fragment_native_tie_normalization_bytes");
    out.fragment_native_exact_final_bytes = required_json_counter(
        stats, "fragment_native_exact_final_bytes");
    out.fragment_native_peak_exact_final_bytes = required_json_counter(
        stats, "fragment_native_peak_exact_final_bytes");
    out.fragment_native_global_sort_calls_invoked = required_json_counter(
        stats, "fragment_native_global_sort_calls_invoked");
    out.fragment_native_global_sort_calls_skipped = required_json_counter(
        stats, "fragment_native_global_sort_calls_skipped");
    out.fragment_native_candidate_anchor_bytes = required_json_counter(
        stats, "fragment_native_candidate_anchor_bytes");
    out.fragment_native_plan_ordinal_bytes = required_json_counter(
        stats, "fragment_native_plan_ordinal_bytes");
    out.fragment_native_compaction_bytes = required_json_counter(
        stats, "fragment_native_compaction_bytes");
    out.fragment_native_final_copy_bytes = required_json_counter(
        stats, "fragment_native_final_copy_bytes");
    out.fragment_native_peak_occurrence_capacity_bytes = required_json_counter(
        stats, "fragment_native_peak_occurrence_capacity_bytes");
    out.fragment_native_peak_query_plan_capacity_bytes = required_json_counter(
        stats, "fragment_native_peak_query_plan_capacity_bytes");
    out.fragment_native_peak_query_group_offset_capacity_bytes =
        required_json_counter(
            stats,
            "fragment_native_peak_query_group_offset_capacity_bytes");
    out.fragment_native_peak_query_membership_filter_capacity_bytes =
        required_json_counter(
            stats,
            "fragment_native_peak_query_membership_filter_capacity_bytes");
    out.fragment_native_peak_query_radix_capacity_bytes = required_json_counter(
        stats, "fragment_native_peak_query_radix_capacity_bytes");
    out.fragment_native_peak_occurrence_count_capacity_bytes =
        required_json_counter(
            stats,
            "fragment_native_peak_occurrence_count_capacity_bytes");
    out.fragment_native_peak_block_layout_capacity_bytes =
        required_json_counter(
            stats, "fragment_native_peak_block_layout_capacity_bytes");
    out.fragment_native_peak_block_stats_capacity_bytes =
        required_json_counter(
            stats, "fragment_native_peak_block_stats_capacity_bytes");
    out.fragment_native_occurrence_allocations = required_json_counter(
        stats, "fragment_native_occurrence_allocations");
    out.fragment_native_query_plan_allocations = required_json_counter(
        stats, "fragment_native_query_plan_allocations");
    out.fragment_native_query_group_offset_allocations = required_json_counter(
        stats, "fragment_native_query_group_offset_allocations");
    out.fragment_native_query_membership_filter_allocations =
        required_json_counter(
            stats,
            "fragment_native_query_membership_filter_allocations");
    out.fragment_native_query_radix_allocations = required_json_counter(
        stats, "fragment_native_query_radix_allocations");
    out.fragment_native_occurrence_count_allocations = required_json_counter(
        stats, "fragment_native_occurrence_count_allocations");
    out.fragment_native_block_layout_allocations = required_json_counter(
        stats, "fragment_native_block_layout_allocations");
    out.fragment_native_block_stats_allocations = required_json_counter(
        stats, "fragment_native_block_stats_allocations");
    out.fragment_native_query_occurrences = required_json_counter(
        stats, "fragment_native_query_occurrences");
    out.fragment_native_unique_query_plans = required_json_counter(
        stats, "fragment_native_unique_query_plans");
    out.fragment_native_packed_occurrence_waves = required_json_counter(
        stats, "fragment_native_packed_occurrence_waves");
    out.fragment_native_wide_occurrence_waves = required_json_counter(
        stats, "fragment_native_wide_occurrence_waves");
    out.fragment_native_compact_direct_lookup_ns = required_json_counter(
        stats, "fragment_native_compact_direct_lookup_ns");
    out.fragment_native_compact_direct_selection_shards =
        required_json_counter(
            stats,
            "fragment_native_compact_direct_selection_shards");
    out.fragment_native_compact_direct_shard_query_layouts =
        required_json_counter(
            stats,
            "fragment_native_compact_direct_shard_query_layouts");
    out.fragment_native_compact_direct_layout_shards_completed =
        required_json_counter(
            stats,
            "fragment_native_compact_direct_layout_shards_completed");
    out.fragment_native_compact_direct_emission_shards_completed =
        required_json_counter(
            stats,
            "fragment_native_compact_direct_emission_shards_completed");
    out.fragment_native_compact_direct_posting_visit_passes =
        required_json_counter(
            stats,
            "fragment_native_compact_direct_posting_visit_passes");
    out.fragment_native_compact_direct_selected_postings =
        required_json_counter(
            stats,
            "fragment_native_compact_direct_selected_postings");
    out.fragment_native_compact_direct_layout_postings_visited =
        required_json_counter(
            stats,
            "fragment_native_compact_direct_layout_postings_visited");
    out.fragment_native_compact_direct_emission_postings_visited =
        required_json_counter(
            stats,
            "fragment_native_compact_direct_emission_postings_visited");
    out.fragment_native_compact_direct_target_control_scan_bytes =
        required_json_counter(
            stats,
            "fragment_native_compact_direct_target_control_scan_bytes");
    out.fragment_native_compact_direct_target_control_scan_passes =
        required_json_counter(
            stats,
            "fragment_native_compact_direct_target_control_scan_passes");
    out.fragment_native_compact_direct_fallbacks = required_json_counter(
        stats, "fragment_native_compact_direct_fallbacks");
    out.fragment_native_compact_direct_traversal_rank_checkpoints_examined =
        required_json_counter(
            stats,
            "fragment_native_compact_direct_traversal_rank_checkpoints_examined");
    out.fragment_native_compact_direct_traversal_membership_words_examined =
        required_json_counter(
            stats,
            "fragment_native_compact_direct_traversal_membership_words_examined");
    out.fragment_native_compact_direct_rejected_posting_reads =
        required_json_counter(
            stats,
            "fragment_native_compact_direct_rejected_posting_reads");
    out.are_fragment_native_stats_saturated =
        stats["are_fragment_native_stats_saturated"].get<bool>() ? 1 : 0;
    out.are_grouped_soa_reader_memory_stats_saturated =
        stats["are_grouped_soa_reader_memory_stats_saturated"].get<bool>()
            ? 1 : 0;
    out.minimizer_fragment_reader_stats = stats;
}

void capture_hash_major_index_reader_stats(
        const it::connection_test_context& connection,
        e2e_output& out) {
    const it::row_t row = it::query_one(
        connection,
        "SELECT bioseq.last_alignment_hash_major_index_reader_stats();");
    if (row.size() != 1) {
        abort_pipeline_stats_capture(
            "hash-major index reader stats have an invalid scalar shape");
    }
    const nlohmann::json stats = parse_exact_json_cell(
        row.front(), "hash-major index reader stats JSON failed to parse");
    const bool header_valid = json_has_exact_keys(
            stats, k_hash_major_index_reader_stats_keys)
        && stats["schema"].is_string()
        && stats["schema"] == "bioseq.hash_major_reader_stats"
        && stats["revision"].is_number_integer()
        && stats["revision"] == 1
        && stats["is_saturated"].is_boolean()
        && stats["has_operational_identity_conflict"].is_boolean()
        && stats["are_stats_saturated"].is_boolean();
    if (!header_valid) {
        abort_pipeline_stats_capture(
            "hash-major index reader stats have the wrong exact schema or types");
    }

    for (const auto key : k_hash_major_index_reader_stats_keys) {
        if (key == "schema"
            || key == "source_identity"
            || key == "source_content_crc64"
            || key == "compatibility_digest"
            || key == "is_saturated"
            || key == "has_operational_identity_conflict"
            || key == "are_stats_saturated") {
            continue;
        }
        (void)required_json_counter(stats, key);
    }
    const std::string& source_identity =
        required_json_text(stats, "source_identity");
    const std::string& source_content_crc64 =
        required_json_text(stats, "source_content_crc64");
    const std::string& compatibility_digest =
        required_json_text(stats, "compatibility_digest");
    if (!fixed_lower_hex(source_identity, 32)
        || !fixed_lower_hex(source_content_crc64, 16)
        || !fixed_lower_hex(compatibility_digest, 64)) {
        abort_pipeline_stats_capture(
            "hash-major index reader stats identity is malformed");
    }

    const std::int64_t execution_id =
        required_json_counter(stats, "execution_id");
    const std::int64_t backend_pid =
        required_json_counter(stats, "backend_pid");
    const std::int64_t database_oid =
        required_json_counter(stats, "database_oid");
    if (execution_id != out.alignment_execution_id
        || backend_pid != out.alignment_backend_pid
        || database_oid != out.alignment_database_oid
        || required_json_counter(stats, "format_revision")
            != (out.pre_restart_persisted_layout == "hash_major" ? 6 : 0)
        || stats["has_operational_identity_conflict"].get<bool>()
            != (out.operational_identity_conflict != 0)
        || stats["are_stats_saturated"].get<bool>()
            != (out.stats_saturated != 0)
        || required_json_counter(stats, "backend_fallbacks")
            != out.backend_fallbacks
        || required_json_counter(stats, "recompute_calls")
            != out.recomputed_target_index_rows) {
        abort_pipeline_stats_capture(
            "hash-major index reader stats identity/counters mismatch");
    }

    out.hash_major_directory_rows =
        required_json_counter(stats, "directory_rows");
    out.hash_major_directory_bytes =
        required_json_counter(stats, "directory_bytes");
    out.hash_major_control_rows =
        required_json_counter(stats, "control_rows");
    out.hash_major_control_bytes =
        required_json_counter(stats, "control_bytes");
    out.hash_major_posting_rows =
        required_json_counter(stats, "posting_rows");
    out.hash_major_posting_bytes =
        required_json_counter(stats, "posting_bytes");
    out.hash_major_posting_ranges_delivered =
        required_json_counter(stats, "posting_ranges_delivered");
    out.hash_major_selected_block_count =
        required_json_counter(stats, "selected_block_count");
    out.hash_major_accepted_hash_count =
        required_json_counter(stats, "accepted_hash_count");
    out.hash_major_retained_position_count =
        required_json_counter(stats, "retained_position_count");
    out.hash_major_authorized_posting_bytes =
        required_json_counter(stats, "authorized_posting_bytes");
    out.hash_major_peak_admitted_bytes =
        required_json_counter(stats, "peak_admitted_bytes");
    out.hash_major_retained_owner_bytes =
        required_json_counter(stats, "retained_owner_bytes");
    out.hash_major_admitted_bytes =
        required_json_counter(stats, "admitted_bytes");
    out.hash_major_failures =
        required_json_counter(stats, "failures");
    out.hash_major_is_saturated = stats["is_saturated"].get<bool>() ? 1 : 0;
    out.hash_major_index_reader_stats = stats;
}

/// Validate and retain the immediate cache-qualified persisted-GPU ledger.
void capture_persisted_gpu_operation_stats(const it::row_t& row, e2e_output& out) {
    if (row.size() != 1) {
        abort_pipeline_stats_capture("persisted GPU operation stats has an invalid scalar shape");
    }
    const nlohmann::json stats =
        parse_exact_json_cell(row.front(), "persisted GPU operation stats JSON failed to parse");
    if (!json_has_exact_keys(stats, k_persisted_gpu_operation_stats_keys) ||
        required_json_text(stats, "schema") != "bioseq.persisted_gpu_operation_stats" ||
        required_json_counter(stats, "revision") != 3) {
        abort_pipeline_stats_capture("persisted GPU operation stats has the wrong exact schema");
    }

    const std::int64_t execution_id = required_json_counter(stats, "execution_id");
    const std::int64_t backend_pid = required_json_counter(stats, "backend_pid");
    const std::int64_t database_oid = required_json_counter(stats, "database_oid");
    const bool completed = required_json_boolean(stats, "is_completed");
    const bool partial = required_json_boolean(stats, "is_partial");
    const bool fully_drained = required_json_boolean(stats, "is_fully_drained");
    const bool producer_consistent = required_json_boolean(stats, "is_consistent");
    const bool complete = required_json_boolean(stats, "is_complete");
    const bool saturated = required_json_boolean(stats, "is_saturated");
    const std::int64_t dropped_operation_count =
        required_json_counter(stats, "dropped_operation_count");
    const std::int64_t operation_count =
        required_json_counter(stats, "operation_count");
    const std::int64_t accepted_operation_count =
        required_json_counter(stats, "accepted_operation_count");
    const std::int64_t complete_operation_count =
        required_json_counter(stats, "complete_operation_count");
    const std::int64_t job_count =
        required_json_counter(stats, "job_count");
    const std::int64_t accepted_job_count =
        required_json_counter(stats, "accepted_job_count");
    const nlohmann::json& operations =
        required_json_array(stats, "operations");
    const bool expects_persisted_gpu = out.actual_backend == "vulkan_gpu"
        && out.actual_target_index_source == "persisted";
    const auto validation_found = stats.find("cache_validation");
    if (validation_found == stats.end()) {
        abort_pipeline_stats_capture(
            "persisted GPU cache validation is absent");
    }

    if (execution_id != out.alignment_execution_id
        || backend_pid != out.alignment_backend_pid
        || database_oid != out.alignment_database_oid
        || completed != (out.alignment_completed != 0)
        || partial != (out.alignment_partial != 0)
        || fully_drained != (out.alignment_fully_drained != 0)) {
        abort_pipeline_stats_capture(
            "persisted GPU operation stats identity/lifecycle differs from the alignment snapshot");
    }
    if (operations.size() > static_cast<std::size_t>(
            (std::numeric_limits<std::int64_t>::max)())
        || operation_count
            != static_cast<std::int64_t>(operations.size())) {
        abort_pipeline_stats_capture(
            "persisted GPU operation stats operation count is inconsistent");
    }


    std::string validation_identity;
    std::string validation_seed_receipt_identity;
    std::string validation_device_uuid;
    std::string validation_pipeline_cache_uuid;
    std::string validation_seed_sha256;
    std::string validation_manifest_checksum;
    std::int64_t validation_api_version = 0;
    std::int64_t validation_driver_version = 0;
    std::int64_t validation_vendor_id = 0;
    std::int64_t validation_device_id = 0;
    std::int64_t validation_seed_size = 0;
    bool cache_validation_equations_valid = !expects_persisted_gpu;
    if (expects_persisted_gpu) {
        if (!validation_found->is_object()
            || !json_has_exact_keys(
                *validation_found,
                k_persisted_gpu_cache_validation_keys)) {
            abort_pipeline_stats_capture(
                "persisted GPU cache validation has the wrong exact schema");
        }
        const nlohmann::json& validation = *validation_found;
        const std::int64_t validation_revision =
            required_json_counter(validation, "revision");
        validation_identity = required_json_hex_identity(
            validation, "identity", 16);
        const bool validation_consistent =
            required_json_boolean(validation, "is_consistent");
        const bool validation_saturated =
            required_json_boolean(validation, "is_saturated");
        const std::int64_t archive_count =
            required_json_counter(validation, "archive_count");
        const std::int64_t archive_bytes =
            required_json_counter(validation, "archive_bytes");
        (void)required_json_hex_identity(
            validation, "archive_checksum", 16);
        const std::int64_t receipt_count =
            required_json_counter(validation, "receipt_count");
        const std::int64_t hash_passes =
            required_json_counter(validation, "hash_passes");
        const std::int64_t hashed_bytes =
            required_json_counter(validation, "hashed_bytes");
        const std::int64_t private_files_created = required_json_counter(
            validation, "private_files_created");
        const std::int64_t private_files_unlinked = required_json_counter(
            validation, "private_files_unlinked");
        const std::int64_t live_private_paths = required_json_counter(
            validation, "live_private_paths");
        const std::int64_t pipeline_filesystem_hash_passes =
            required_json_counter(
                validation, "pipeline_filesystem_hash_passes");
        const std::int64_t pipeline_filesystem_hashed_bytes =
            required_json_counter(
                validation, "pipeline_filesystem_hashed_bytes");
        const std::int64_t helper_protocol_revision = required_json_counter(
            validation, "helper_protocol_revision");
        const std::string& helper_build_identity = required_json_text(
            validation, "helper_build_identity");
        const std::string& helper_source =
            required_json_text(validation, "helper_source");
        const std::int64_t helper_executable_size = required_json_counter(
            validation, "helper_executable_size");
        (void)required_json_hex_identity(
            validation, "helper_executable_sha256", 64);
        (void)required_json_hex_identity(
            validation, "helper_metal_registry_id", 16);
        validation_device_uuid = required_json_hex_identity(
            validation, "device_uuid", 32);
        validation_api_version =
            required_json_counter(validation, "api_version");
        validation_driver_version =
            required_json_counter(validation, "driver_version");
        validation_vendor_id =
            required_json_counter(validation, "vendor_id");
        validation_device_id =
            required_json_counter(validation, "device_id");
        validation_pipeline_cache_uuid = required_json_hex_identity(
            validation, "pipeline_cache_uuid", 32);
        validation_seed_receipt_identity = required_json_hex_identity(
            validation, "seed_receipt_identity", 16);
        validation_seed_sha256 = required_json_hex_identity(
            validation, "seed_sha256", 64);
        validation_seed_size =
            required_json_counter(validation, "seed_size");
        validation_manifest_checksum = required_json_hex_identity(
            validation, "manifest_checksum", 16);

        const bool helper_build_valid = helper_build_identity
            == "bioseq_gpu_archive_builder";
        const bool numeric_driver_identity_valid =
            validation_api_version > 0
            && validation_driver_version > 0
            && validation_api_version
                <= (std::numeric_limits<std::uint32_t>::max)()
            && validation_driver_version
                <= (std::numeric_limits<std::uint32_t>::max)()
            && validation_vendor_id
                <= (std::numeric_limits<std::uint32_t>::max)()
            && validation_device_id
                <= (std::numeric_limits<std::uint32_t>::max)();
        const std::string canonical_driver = numeric_driver_identity_valid
            ? fmt::format(
                "api={:08x};driver={:08x};vendor={:08x};device={:08x};pcuuid={}",
                static_cast<std::uint32_t>(validation_api_version),
                static_cast<std::uint32_t>(validation_driver_version),
                static_cast<std::uint32_t>(validation_vendor_id),
                static_cast<std::uint32_t>(validation_device_id),
                validation_pipeline_cache_uuid)
            : std::string{};
        cache_validation_equations_valid =
            validation_revision == 2 && validation_consistent && !validation_saturated &&
            archive_count > 0 && archive_bytes > 0 && receipt_count == archive_count &&
            hash_passes == 1 && hashed_bytes == archive_bytes &&
            private_files_created == archive_count && private_files_unlinked == archive_count &&
            live_private_paths == 0 && pipeline_filesystem_hash_passes == 0 &&
            pipeline_filesystem_hashed_bytes == 0 && helper_protocol_revision == 2 &&
            helper_build_valid && helper_source == "installed-sibling" &&
            helper_executable_size > 0 && numeric_driver_identity_valid &&
            validation_device_uuid == out.gpu_device_uuid && canonical_driver == out.gpu_driver &&
            validation_seed_sha256 == out.gpu_pipeline_cache_seed_sha256 &&
            validation_seed_size == out.gpu_pipeline_cache_seed_size &&
            validation_manifest_checksum == out.gpu_pipeline_cache_manifest_checksum;
    } else if (!validation_found->is_null()) {
        abort_pipeline_stats_capture("non-persisted-GPU stats retained cache validation");
    }

    bool operation_equations_valid = true;
    __int128 expected_job_sum = 0;
    __int128 accepted_job_sum = 0;
    __int128 cache_load_sum = 0;
    __int128 cache_seed_bytes_sum = 0;
    __int128 cache_load_ns_sum = 0;
    __int128 pipeline_create_sum = 0;
    __int128 pipeline_creation_ns_sum = 0;
    __int128 alignment_submission_sum = 0;
    __int128 mapq_submission_sum = 0;
    std::set<std::string> operation_identities;
    std::set<std::string> pipeline_operation_identities;
    std::set<std::tuple<std::string, std::int64_t, std::int64_t>> logical_jobs;
    std::optional<std::string> common_seed_receipt_identity;
    std::optional<std::string> common_cold_load_identity;
    bool output_coordinate_witness = false;

    for (const nlohmann::json& operation : operations) {
        if (!json_has_exact_keys(operation, k_persisted_gpu_operation_keys)) {
            abort_pipeline_stats_capture("persisted GPU operation has the wrong exact schema");
        }
        const std::string& operation_identity =
            required_json_hex_identity(operation, "operation_identity", 16);
        const std::int64_t expected_jobs = required_json_counter(operation, "expected_jobs");
        const std::int64_t validated_jobs = required_json_counter(operation, "validated_jobs");
        const std::int64_t consumed_jobs = required_json_counter(operation, "consumed_jobs");
        const std::int64_t accepted_jobs = required_json_counter(operation, "accepted_jobs");
        const std::int64_t burned_jobs = required_json_counter(operation, "burned_jobs");
        const std::int64_t pending_jobs = required_json_counter(operation, "pending_jobs");
        const std::int64_t staged_jobs = required_json_counter(operation, "staged_jobs");
        const std::int64_t published_jobs = required_json_counter(operation, "published_jobs");
        const std::int64_t publication_commits =
            required_json_counter(operation, "publication_commits");
        const std::int64_t query_receipts =
            required_json_counter(operation, "query_pipeline_receipt_count");
        const std::int64_t shape_receipts =
            required_json_counter(operation, "shape_pipeline_receipt_count");
        const std::int64_t alignment_receipts =
            required_json_counter(operation, "alignment_pipeline_receipt_count");
        const std::int64_t alignment_submissions =
            required_json_counter(operation, "alignment_submission_count");
        const std::int64_t mapq_receipts =
            required_json_counter(operation, "mapq_pipeline_receipt_count");
        const std::int64_t mapq_submissions =
            required_json_counter(operation, "mapq_submission_count");
        const std::int64_t mapq_results =
            required_json_counter(operation, "mapq_result_count");
        const std::int64_t mapq_published =
            required_json_counter(operation, "mapq_published_count");
        const std::string& mapq_result_digest =
            required_json_text(operation, "mapq_result_digest");
        if (!fixed_lower_hex(mapq_result_digest, 16)) {
            abort_pipeline_stats_capture(
                "persisted GPU MAPQ result digest is malformed");
        }
        const std::int64_t submission_count = required_json_counter(operation, "submission_count");
        const std::string& query_operation_identity =
            required_json_hex_identity(operation, "query_pipeline_operation_identity", 16);
        const std::string& shape_operation_identity =
            required_json_hex_identity(operation, "shape_pipeline_operation_identity", 16);
        const nlohmann::json& query_submission_ordinals =
            required_json_array(operation, "query_submission_ordinals");
        const std::string& seed_receipt_identity =
            required_json_hex_identity(operation, "seed_receipt_identity", 16);
        const std::string& cache_token_identity =
            required_json_hex_identity(operation, "cache_token_identity", 16);
        const std::string& cache_validation_identity =
            required_json_hex_identity(operation, "cache_validation_identity", 16);
        const std::int64_t generation_id = required_json_counter(operation, "generation_id");
        const std::string& source_identity =
            required_json_hex_identity(operation, "source_identity", 16);
        const std::string& cold_load_identity =
            required_json_hex_identity(operation, "cold_load_identity", 16);
        const std::string& source = required_json_text(operation, "source");
        const nlohmann::json& environment = required_json_object(operation, "environment");
        const nlohmann::json& cache_metrics = required_json_object(operation, "cache_metrics");
        const nlohmann::json& jobs = required_json_array(operation, "jobs");

        if (!json_has_exact_keys(environment, k_persisted_gpu_environment_keys) ||
            !json_has_exact_keys(cache_metrics, k_persisted_gpu_cache_metrics_keys)) {
            abort_pipeline_stats_capture(
                "persisted GPU operation environment/cache schema is invalid");
        }
        const std::string& device_uuid = required_json_hex_identity(environment, "device_uuid", 32);
        const std::int64_t api_version = required_json_counter(environment, "api_version");
        const std::int64_t driver_version = required_json_counter(environment, "driver_version");
        const std::int64_t vendor_id = required_json_counter(environment, "vendor_id");
        const std::int64_t device_id = required_json_counter(environment, "device_id");
        const std::string& pipeline_cache_uuid =
            required_json_hex_identity(environment, "pipeline_cache_uuid", 32);
        const std::string& seed_sha256 = required_json_hex_identity(environment, "seed_sha256", 64);
        const std::int64_t seed_size = required_json_counter(environment, "seed_size");
        const std::string& manifest_checksum =
            required_json_hex_identity(environment, "manifest_checksum", 16);

        const bool numeric_driver_identity_valid =
            api_version > 0 && driver_version > 0 &&
            api_version <= (std::numeric_limits<std::uint32_t>::max)() &&
            driver_version <= (std::numeric_limits<std::uint32_t>::max)() &&
            vendor_id <= (std::numeric_limits<std::uint32_t>::max)() &&
            device_id <= (std::numeric_limits<std::uint32_t>::max)();
        const std::string canonical_driver =
            numeric_driver_identity_valid
                ? fmt::format("api={:08x};driver={:08x};vendor={:08x};device={:08x};pcuuid={}",
                              static_cast<std::uint32_t>(api_version),
                              static_cast<std::uint32_t>(driver_version),
                              static_cast<std::uint32_t>(vendor_id),
                              static_cast<std::uint32_t>(device_id),
                              pipeline_cache_uuid)
                : std::string{};
        const bool environment_valid =
            numeric_driver_identity_valid && device_uuid == out.gpu_device_uuid &&
            canonical_driver == out.gpu_driver &&
            seed_sha256 == out.gpu_pipeline_cache_seed_sha256 &&
            seed_size == out.gpu_pipeline_cache_seed_size &&
            manifest_checksum == out.gpu_pipeline_cache_manifest_checksum;

        const std::int64_t cache_loads =
            required_json_counter(cache_metrics, "pipeline_cache_loads");
        const std::int64_t cache_seed_bytes =
            required_json_counter(cache_metrics, "pipeline_cache_seed_bytes");
        const std::int64_t cache_load_ns =
            required_json_counter(cache_metrics, "pipeline_cache_load_ns");
        const std::int64_t pipeline_creates =
            required_json_counter(cache_metrics, "pipeline_creates");
        const std::int64_t pipeline_creation_ns =
            required_json_counter(cache_metrics, "pipeline_creation_ns");
        const bool authenticated_alignment_pipeline_present =
            out.observed_base_alignment_algorithm == "vulkan_simple_cigar" &&
            out.gpu_extend_dispatches > 0 && alignment_receipts == 1 && alignment_submissions > 0;
        const bool mapq_digest_present =
            mapq_result_digest != "0000000000000000";
        const bool authenticated_mapq_pipeline_present =
            out.actual_backend == "vulkan_gpu" && out.mapq_executed == 1
            && mapq_receipts == 1 && mapq_submissions == 1
            && mapq_published <= mapq_results && mapq_digest_present;
        const bool mapq_pipeline_shape_valid = out.mapq_executed == 1
            ? authenticated_mapq_pipeline_present
            : mapq_receipts == 0 && mapq_submissions == 0
                && mapq_results == 0 && mapq_published == 0
                && !mapq_digest_present;
        const bool cache_shape_valid =
            cache_loads <= 1 &&
            (cache_loads == 0 ? cache_seed_bytes == 0 && cache_load_ns == 0
                              : cache_seed_bytes == seed_size && cache_load_ns > 0) &&
            static_cast<__int128>(pipeline_creates) <=
                static_cast<__int128>(shape_receipts) + 1 +
                    (authenticated_alignment_pipeline_present ? 1 : 0)
                    + (authenticated_mapq_pipeline_present ? 1 : 0) &&
            (pipeline_creates == 0 ? pipeline_creation_ns == 0 : pipeline_creation_ns > 0);
        cache_load_sum += cache_loads;
        cache_seed_bytes_sum += cache_seed_bytes;
        cache_load_ns_sum += cache_load_ns;
        pipeline_create_sum += pipeline_creates;
        pipeline_creation_ns_sum += pipeline_creation_ns;
        alignment_submission_sum += alignment_submissions;
        mapq_submission_sum += mapq_submissions;

        std::set<std::pair<std::string, std::int64_t>> structured_submissions;
        std::map<std::pair<std::string, std::int64_t>, std::pair<std::size_t, std::string>>
            shape_submission_receipts;
        std::map<std::string, std::pair<std::string, std::int64_t>> submit_wait_domains;
        bool query_submissions_valid = query_submission_ordinals.size() == 2;
        for (const nlohmann::json& ordinal_json : query_submission_ordinals) {
            const std::int64_t ordinal = required_json_integer_value(ordinal_json);
            query_submissions_valid =
                query_submissions_valid && ordinal > 0 &&
                structured_submissions.emplace(query_operation_identity, ordinal).second;
        }

        bool jobs_valid =
            jobs.size() <= static_cast<std::size_t>((std::numeric_limits<std::int64_t>::max)()) &&
            expected_jobs == static_cast<std::int64_t>(jobs.size());
        constexpr std::array<std::string_view, 4> expected_submission_stages{
            "count",
            "emit",
            "sort_range",
            "chain_coordinate",
        };
        for (const nlohmann::json& job : jobs) {
            if (!json_has_exact_keys(job, k_persisted_gpu_job_keys)) {
                abort_pipeline_stats_capture("persisted GPU job has the wrong exact schema");
            }
            const std::int64_t logical_target_row =
                required_json_counter(job, "logical_target_row");
            const std::int64_t logical_query_ordinal =
                required_json_counter(job, "logical_query_ordinal");
            const std::int64_t query_index = required_json_counter(job, "query_index");
            const std::int64_t query_length = required_json_counter(job, "query_length");
            const std::int64_t query_minimizer_count =
                required_json_counter(job, "query_minimizer_count");
            const std::int64_t query_tile_count = required_json_counter(job, "query_tile_count");
            const nlohmann::json& query = required_json_object(job, "query");
            const nlohmann::json& direct = required_json_object(job, "direct");
            const nlohmann::json& submissions = required_json_array(job, "submissions");
            const nlohmann::json& status = required_json_object(job, "status");
            const nlohmann::json& forbidden = required_json_object(job, "forbidden");
            const nlohmann::json& coordinate = required_json_object(job, "coordinate");
            if (!json_has_exact_keys(query, k_persisted_gpu_query_keys) ||
                !json_has_exact_keys(direct, k_persisted_gpu_direct_keys) ||
                !json_has_exact_keys(status, k_persisted_gpu_status_keys) ||
                !json_has_exact_keys(forbidden, k_persisted_gpu_forbidden_keys) ||
                !json_has_exact_keys(coordinate, k_persisted_gpu_coordinate_keys)) {
                abort_pipeline_stats_capture("persisted GPU job nested object schema is invalid");
            }

            const bool query_valid = query_length > 0 && query_minimizer_count > 0 &&
                                     query_tile_count > 0 &&
                                     required_json_counter(query, "count_dispatches") == 1 &&
                                     required_json_counter(query, "scan_dispatches") == 3 &&
                                     required_json_counter(query, "emit_dispatches") == 1 &&
                                     required_json_counter(query, "completed_roundtrips") == 2;
            const std::int64_t anchor_count = required_json_counter(direct, "anchor_count");
            const std::int64_t range_count = required_json_counter(direct, "range_count");
            const bool direct_valid =
                required_json_counter(direct, "count_dispatches") == 1 &&
                required_json_counter(direct, "scan_dispatches") == 3 && anchor_count > 0 &&
                required_json_counter(direct, "emit_dispatches") == 1 &&
                required_json_counter(direct, "sort_operations") == 1 &&
                required_json_counter(direct, "range_build_dispatches") == 1 && range_count > 0 &&
                required_json_counter(direct, "chain_dispatches") == 1 &&
                required_json_counter(direct, "chain_backtrack_dispatches") == 1 &&
                required_json_counter(direct, "coordinate_dispatches") == 1;

            bool submissions_valid = submissions.size() == expected_submission_stages.size();
            std::set<std::int64_t> job_submission_ordinals;
            std::set<std::string> job_submit_wait_identities;
            for (std::size_t submission_index = 0; submission_index < submissions.size();
                 ++submission_index) {
                const nlohmann::json& submission = submissions[submission_index];
                if (!json_has_exact_keys(submission, k_persisted_gpu_submission_keys)) {
                    abort_pipeline_stats_capture(
                        "persisted GPU submission has the wrong exact schema");
                }
                const std::string& stage = required_json_text(submission, "stage");
                const std::string& submission_operation_identity =
                    required_json_hex_identity(submission, "operation_identity", 16);
                const std::int64_t ordinal = required_json_counter(submission, "ordinal");
                const std::string& submit_wait_identity =
                    required_json_hex_identity(submission, "submit_wait_identity", 16);
                const auto submission_domain = std::pair{submission_operation_identity, ordinal};
                const auto [receipt, receipt_inserted] = shape_submission_receipts.emplace(
                    submission_domain, std::pair{submission_index, submit_wait_identity});
                const bool receipt_consistent =
                    receipt_inserted ||
                    receipt->second == std::pair{submission_index, submit_wait_identity};
                const auto [wait_domain, wait_inserted] =
                    submit_wait_domains.emplace(submit_wait_identity, submission_domain);
                const bool wait_consistent =
                    wait_inserted || wait_domain->second == submission_domain;
                submissions_valid =
                    submissions_valid && submission_index < expected_submission_stages.size() &&
                    stage == expected_submission_stages[submission_index] &&
                    submission_operation_identity == shape_operation_identity && ordinal > 0 &&
                    job_submission_ordinals.emplace(ordinal).second &&
                    job_submit_wait_identities.emplace(submit_wait_identity).second &&
                    receipt_consistent && wait_consistent;
                structured_submissions.emplace(submission_operation_identity, ordinal);
            }

            const bool status_valid = required_json_counter(status, "count_checks") == 1 &&
                                      required_json_counter(status, "emit_checks") == 1 &&
                                      required_json_counter(status, "status_or") == 0 &&
                                      required_json_counter(status, "result_readbacks") == 1;
            const bool forbidden_valid =
                required_json_counter(forbidden, "coordinate_vote_dispatches") == 0 &&
                required_json_counter(forbidden, "cpu_or_simd_biological_calls") == 0 &&
                required_json_counter(forbidden, "recompute_calls") == 0 &&
                required_json_counter(forbidden, "fallback_calls") == 0;

            const bool coordinate_valid_flag = required_json_boolean(coordinate, "valid");
            const std::int64_t query_start = required_json_counter(coordinate, "query_start");
            const std::int64_t query_end = required_json_counter(coordinate, "query_end");
            const std::int64_t target_start = required_json_counter(coordinate, "target_start");
            const std::int64_t target_end = required_json_counter(coordinate, "target_end");
            const std::int64_t match_length = required_json_counter(coordinate, "match_length");
            const std::int64_t block_length = required_json_counter(coordinate, "block_length");
            const std::int64_t chain_score = required_json_counter(coordinate, "chain_score");
            const std::int64_t seed_count = required_json_counter(coordinate, "seed_count");
            const bool is_reverse = required_json_boolean(coordinate, "is_reverse");
            const bool coordinate_valid =
                coordinate_valid_flag && query_start < query_end && query_end <= query_length &&
                target_start < target_end && match_length > 0 && match_length <= block_length &&
                match_length <= query_end - query_start &&
                match_length <= target_end - target_start &&
                static_cast<__int128>(block_length) <=
                    static_cast<__int128>(query_end - query_start) +
                        static_cast<__int128>(target_end - target_start) &&
                chain_score > 0 && seed_count > 0;

            if (query_index == 0 && target_start == out.target_start &&
                is_reverse == out.is_reverse) {
                output_coordinate_witness = true;
            }

            jobs_valid =
                jobs_valid &&
                logical_jobs.emplace(operation_identity, logical_target_row, logical_query_ordinal)
                    .second &&
                query_index < expected_jobs && query_valid && direct_valid && submissions_valid &&
                status_valid && forbidden_valid && coordinate_valid;
        }

        const bool seed_receipt_matches = !common_seed_receipt_identity.has_value() ||
                                          *common_seed_receipt_identity == seed_receipt_identity;
        const bool cold_load_matches = !common_cold_load_identity.has_value() ||
                                       *common_cold_load_identity == cold_load_identity;
        if (!common_seed_receipt_identity.has_value()) {
            common_seed_receipt_identity = seed_receipt_identity;
        }
        if (!common_cold_load_identity.has_value()) {
            common_cold_load_identity = cold_load_identity;
        }
        const bool identity_valid =
            operation_identities.emplace(operation_identity).second &&
            pipeline_operation_identities.emplace(query_operation_identity).second &&
            pipeline_operation_identities.emplace(shape_operation_identity).second &&
            operation_identity != query_operation_identity &&
            operation_identity == shape_operation_identity &&
            query_operation_identity != shape_operation_identity &&
            cache_token_identity == operation_identity &&
            cache_validation_identity == validation_identity &&
            seed_receipt_identity == validation_seed_receipt_identity && seed_receipt_matches &&
            cold_load_matches && generation_id == out.persisted_generation_id &&
            source_identity == out.persisted_source_identity && source == "persisted_grouped_soa" &&
            device_uuid == validation_device_uuid && api_version == validation_api_version &&
            driver_version == validation_driver_version && vendor_id == validation_vendor_id &&
            device_id == validation_device_id &&
            pipeline_cache_uuid == validation_pipeline_cache_uuid &&
            seed_sha256 == validation_seed_sha256 && seed_size == validation_seed_size &&
            manifest_checksum == validation_manifest_checksum;
        const bool publication_valid =
            expected_jobs > 0 && validated_jobs == expected_jobs &&
            consumed_jobs == expected_jobs && accepted_jobs == expected_jobs && burned_jobs == 0 &&
            pending_jobs == 0 && staged_jobs == expected_jobs && published_jobs == expected_jobs &&
            publication_commits == 1 && query_receipts == 3 && shape_receipts >= 7 &&
            shape_receipts <= 10 && alignment_receipts >= 0 && alignment_receipts <= 1 &&
            (alignment_receipts == 0) == (alignment_submissions == 0) &&
            (out.observed_base_alignment_algorithm == "vulkan_simple_cigar"
                 ? authenticated_alignment_pipeline_present
                 : alignment_receipts == 0) &&
            mapq_pipeline_shape_valid &&
            query_submissions_valid && submission_count > 0 &&
            structured_submissions.size() <=
                static_cast<std::size_t>((std::numeric_limits<std::int64_t>::max)()) &&
            static_cast<__int128>(submission_count) ==
                static_cast<__int128>(structured_submissions.size())
                    + static_cast<__int128>(alignment_submissions)
                    + static_cast<__int128>(mapq_submissions);
        operation_equations_valid = operation_equations_valid && identity_valid &&
                                    publication_valid && environment_valid && cache_shape_valid &&
                                    jobs_valid;
        expected_job_sum += expected_jobs;
        accepted_job_sum += accepted_jobs;
    }

    const auto counter = [&out](const std::size_t column) noexcept {
        return out.alignment_snapshot_numeric_columns[column];
    };
    const bool aggregate_cache_valid =
        cache_load_sum == static_cast<__int128>(counter(124)) &&
        cache_seed_bytes_sum == static_cast<__int128>(counter(125)) &&
        cache_load_ns_sum == static_cast<__int128>(counter(126)) &&
        pipeline_creation_ns_sum == static_cast<__int128>(counter(127)) &&
        pipeline_create_sum == static_cast<__int128>(counter(138));
    const bool accepted_equations =
        producer_consistent && completed && !partial && fully_drained && complete && !saturated &&
        dropped_operation_count == 0 && operation_count > 0 &&
        accepted_operation_count == operation_count &&
        complete_operation_count == operation_count &&
        expected_job_sum == static_cast<__int128>(job_count) &&
        accepted_job_sum == static_cast<__int128>(accepted_job_count) &&
        job_count == out.attempted_alignment_jobs &&
        accepted_job_count == out.fully_attested_alignment_jobs &&
        out.unattested_alignment_jobs == 0 && output_coordinate_witness &&
        alignment_submission_sum == static_cast<__int128>(out.gpu_extend_dispatches) &&
        (out.mapq_executed == 1
             ? mapq_submission_sum == operation_count
             : mapq_submission_sum == 0) &&
        cache_validation_equations_valid && operation_equations_valid && aggregate_cache_valid;
    const bool exact_empty_non_gpu_stats =
        !producer_consistent && complete && !saturated && dropped_operation_count == 0 &&
        operation_count == 0 && accepted_operation_count == 0 && complete_operation_count == 0 &&
        job_count == 0 && accepted_job_count == 0 && operations.empty();
    if (!expects_persisted_gpu && !exact_empty_non_gpu_stats) {
        abort_pipeline_stats_capture("non-persisted-GPU snapshot retained stale operation stats");
    }

    out.persisted_gpu_operation_stats_schema_valid = 1;
    out.persisted_gpu_operation_stats_accepted =
        expects_persisted_gpu && accepted_equations ? 1 : 0;
    out.persisted_gpu_operation_stats_complete = complete ? 1 : 0;
    out.persisted_gpu_operation_stats_saturated = saturated ? 1 : 0;
    out.persisted_gpu_dropped_operation_count = dropped_operation_count;
    out.persisted_gpu_operation_count = operation_count;
    out.persisted_gpu_accepted_operation_count = accepted_operation_count;
    out.persisted_gpu_job_count = job_count;
    out.persisted_gpu_accepted_job_count = accepted_job_count;
    out.persisted_gpu_operation_stats = stats;
}

void capture_alignment_statement_stats(const it::connection_test_context& connection,
                                       const alignment_snapshot_identity& prior,
                                       e2e_output& out,
                                       const bool enforce_measured_envelope = true) {
    constexpr std::size_t k_column_count =
        e2e_output::k_alignment_snapshot_column_count;
    constexpr std::size_t k_stage_count = 20;
    const std::array<std::string_view, k_stage_count> expected_stages{
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
    // This must be the very next SQL statement on the same backend. Retain
    // its scalar value now, before any other diagnostic query can obscure the
    // one-shot cache-qualified ledger bound to the execution identity.
    const it::row_t persisted_gpu_operation_row = it::query_one(
        connection,
        "SELECT bioseq."
        "last_alignment_persisted_gpu_operation_stats();");
    if (rows.size() != k_stage_count) {
        abort_pipeline_stats_capture(
            "alignment snapshot did not return exactly 20 stage rows");
    }
    for (std::size_t row_no = 0; row_no < rows.size(); ++row_no) {
        const auto& row = rows[row_no];
        if (row.size() != k_column_count
            || !row[23].has_value()
            || *row[23] != expected_stages[row_no]) {
            abort_pipeline_stats_capture(
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
                    abort_pipeline_stats_capture(
                        "alignment snapshot contains an ill-typed Boolean");
                }
            } else if (!parse_snapshot_i64(row[column]).has_value()) {
                abort_pipeline_stats_capture(
                    "alignment snapshot contains an ill-typed integer");
            }
        }
        if (row_no != 0) {
            for (std::size_t column = 0; column < row.size(); ++column) {
                const bool stage_local = column >= 23 && column <= 31;
                if (!stage_local && row[column] != rows.front()[column]) {
                    abort_pipeline_stats_capture(
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
            abort_pipeline_stats_capture(
                "alignment snapshot required integer is NULL/invalid");
        }
        return *parsed;
    };
    const auto required_bool = [&rows](const std::size_t column) {
        const auto parsed = parse_snapshot_bool(rows.front()[column]);
        if (!parsed.has_value()) {
            abort_pipeline_stats_capture(
                "alignment snapshot required Boolean is NULL/invalid");
        }
        return *parsed;
    };
    for (std::size_t column = 0; column < k_column_count; ++column) {
        if (alignment_stats_text_column(column)
            || alignment_stats_json_column(column)
            || !rows.front()[column].has_value()) {
            continue;
        }
        const auto parsed = alignment_stats_boolean_column(column)
            ? parse_snapshot_bool(rows.front()[column])
            : parse_snapshot_i64(rows.front()[column]);
        if (!parsed.has_value()) {
            abort_pipeline_stats_capture(
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
        abort_pipeline_stats_capture(
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
    // JSON interval origins depend on the executed backend from this snapshot.
    capture_alignment_stats_json(rows, out);
    capture_persisted_gpu_operation_stats(
        persisted_gpu_operation_row, out);
    capture_minimizer_fragment_reader_stats(connection, out);
    capture_hash_major_index_reader_stats(connection, out);
    out.query_sketch_host_wall_ns = required_i64(30);
    out.target_index_build_host_wall_ns = [&rows] {
        const auto parsed = parse_snapshot_i64(rows[2][30]);
        if (!parsed.has_value()) {
            abort_pipeline_stats_capture(
                "target-index stage timing is NULL/invalid");
        }
        return *parsed;
    }();
    if (out.downstream_active_wall_available == 1
        && enforce_measured_envelope) {
        if (out.downstream_active_wall_ns < 0 ||
            static_cast<std::uint64_t>(out.downstream_active_wall_ns) >
                out.measured_region_ns) {
            abort_pipeline_stats_capture(
                "downstream active-wall union exceeds the same measured run");
        }
    } else if (out.downstream_active_wall_available != 1) {
        out.downstream_active_wall_ns = -1;
    }

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
    const bool gpu_bearing_backend = out.actual_backend == "vulkan_gpu"
        || out.actual_backend == "hybrid_cpu_vulkan";
    const bool environment_valid = out.effective_cpu_workers > 0
        && ((gpu_bearing_backend && gpu_environment)
            || (!gpu_bearing_backend && non_gpu_environment));
    out.environment_mixed_invalid = environment_valid ? 0 : 1;
    out.pipeline_stats_available = 1;
    out.pipeline_stats_attached_to_measurement = 1;
}

// Parse one exact primary PAF row per query. Every accepted row must carry a
// cg:Z CIGAR which replays over the reported half-open query/target spans.
void parse_minimap2_paf(const std::string& paf, const e2e_input& in, e2e_output& out) {
    out.correctness_evaluated = 1;
    auto query_id = [](std::string_view qname) -> std::int64_t {
        constexpr std::string_view prefix = "query";
        if (!qname.starts_with(prefix)) return -1;
        const auto parsed = parse_i64_exact(qname.substr(prefix.size()));
        return parsed.value_or(-1);
    };
    std::vector<std::uint8_t> primary_counts(in.queries.size(), 0);
    std::vector<std::optional<double>> offsets(in.queries.size());
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
        std::int64_t exact_mapq = -1;
        if (in.mapq_projection) {
            const auto mapq = parse_i64_exact(col[11]);
            if (!mapq.has_value() || *mapq < 0
                || *mapq > k_max_mapping_quality) {
                malformed_or_extra = true;
                continue;
            }
            exact_mapq = *mapq;
            ++out.mapq_result_count;
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
        ++out.ordered_query_oracle[qid].primary_match_count;
        if (in.paired_minimap2_oracle) {
            out.ordered_query_oracle[qid].mapq = exact_mapq;
        }
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
        if (offset < static_cast<double>(k_position_tolerance_bp)
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
        if (!offsets[i].has_value()) continue;
        const double offset = *offsets[i];
        out.max_abs_offset_bp = std::max(out.max_abs_offset_bp, offset);
        if (primary_counts[i] == 1
            && offset < static_cast<double>(k_position_tolerance_bp)) {
            ++out.correct_match_count;
        }
    }
    out.position_offset_available = !offsets.empty()
        && std::ranges::all_of(offsets, [](const auto& offset) noexcept {
            return offset.has_value() && std::isfinite(*offset);
        });
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
                && out.cigar_replay_count == out.expected_match_count))
        && (!in.mapq_projection
            || out.mapq_result_count == out.expected_match_count);
    out.abs_offset_bp = out.max_abs_offset_bp;
}

[[nodiscard]] bool mapped_result_is_valid(
        const e2e_input& in,
        const e2e_output& out) noexcept {
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
    const bool mapq_ok = !in.mapq_projection ||
                         out.mapq_result_count == out.expected_match_count;
    return out.found
        && exact_rows
        && all_expected_correct
        && cigar_ok
        && mapq_ok
        && out.position_offset_available
        && out.abs_offset_bp < static_cast<double>(k_position_tolerance_bp);
}

/** @brief Render an unavailable or non-finite mapping offset as JSON-style null. */
[[nodiscard]] std::string position_offset_diagnostic(
        const e2e_output& out) {
    if (!out.position_offset_available || !std::isfinite(out.abs_offset_bp)) {
        return "null";
    }
    return fmt::format("{:.1f}", out.abs_offset_bp);
}

inline void reject_invalid_matrix_result(
        const e2e_input& in,
        e2e_output& out) {
    if (mapped_result_is_valid(in, out)) return;
    out.tuple_accepted = 0;
    out.phase = "correctness";
    out.cell_disposition = "invalid";
    if (out.match_count != out.expected_match_count) {
        out.tuple_rejection_reason = fmt::format(
            "**INVALID**: match-count mismatch observed={}/expected={}",
            out.match_count,
            out.expected_match_count);
    } else {
        out.tuple_rejection_reason = fmt::format(
            "**INVALID**: correctness mismatch match_count={}/{} correct={} strand={} "
            "cigar={}/{} mapq={} offset_bp={}",
            out.match_count,
            out.expected_match_count,
            out.correct_match_count,
            out.correct_strand_count,
            out.correct_cigar_count,
            out.cigar_replay_count,
            out.mapq_result_count,
            position_offset_diagnostic(out));
    }
}

[[nodiscard]] std::string paired_oracle_mismatch_diagnostic(
        const e2e_output& bioseq_output,
        const e2e_output& minimap2_output,
        const std::size_t repeat) {
    if (bioseq_output.tuple_accepted != 1
        || minimap2_output.tuple_accepted != 1) {
        return fmt::format(
            "**INVALID**: paired minimap2 oracle repeat={} has a failed "
            "logical row bioseq_status={} minimap2_status={}",
            repeat, bioseq_output.cell_disposition,
            minimap2_output.cell_disposition);
    }
    if (bioseq_output.match_count != minimap2_output.match_count) {
        return fmt::format(
            "**INVALID**: paired minimap2 oracle repeat={} total primary "
            "match-count mismatch bioseq={} minimap2={} bioseq_sha256={} "
            "minimap2_sha256={}",
            repeat, bioseq_output.match_count, minimap2_output.match_count,
            bioseq_output.ordered_query_oracle_sha256,
            minimap2_output.ordered_query_oracle_sha256);
    }
    const auto& bioseq = bioseq_output.ordered_query_oracle;
    const auto& minimap2 = minimap2_output.ordered_query_oracle;
    if (bioseq.size() != minimap2.size()) {
        return fmt::format(
            "**INVALID**: paired minimap2 oracle repeat={} ordered query "
            "count mismatch bioseq={} minimap2={} bioseq_sha256={} "
            "minimap2_sha256={}",
            repeat, bioseq.size(), minimap2.size(),
            bioseq_output.ordered_query_oracle_sha256,
            minimap2_output.ordered_query_oracle_sha256);
    }
    for (std::size_t index = 0; index < bioseq.size(); ++index) {
        const auto& lhs = bioseq[index];
        const auto& rhs = minimap2[index];
        if (lhs.query_id == rhs.query_id
            && lhs.primary_match_count == rhs.primary_match_count) {
            continue;
        }
        const std::string_view field = lhs.query_id != rhs.query_id
            ? "query_id" : "primary_match_count";
        return fmt::format(
            "**INVALID**: paired minimap2 oracle repeat={} ordered_index={} "
            "field={} bioseq=(query_id={},primary_match_count={}) "
            "minimap2=(query_id={},primary_match_count={}) "
            "bioseq_sha256={} minimap2_sha256={}",
            repeat, index, field,
            lhs.query_id, lhs.primary_match_count,
            rhs.query_id, rhs.primary_match_count,
            bioseq_output.ordered_query_oracle_sha256,
            minimap2_output.ordered_query_oracle_sha256);
    }
    return {};
}

[[nodiscard]] std::pair<bool, std::string>
paired_mapq_diagnostic_comparison(
        const e2e_output& bioseq_output,
        const e2e_output& minimap2_output,
        const std::size_t repeat) {
    const auto& bioseq = bioseq_output.ordered_query_oracle;
    const auto& minimap2 = minimap2_output.ordered_query_oracle;
    if (bioseq.size() != minimap2.size()) {
        return {false, fmt::format(
            "diagnostic MAPQ vector shape differs at repeat={} bioseq={} "
            "minimap2={}; numeric minimap2 MAPQ equivalence is not claimed",
            repeat, bioseq.size(), minimap2.size())};
    }
    for (std::size_t index = 0; index < bioseq.size(); ++index) {
        if (bioseq[index].query_id == minimap2[index].query_id
            && bioseq[index].mapq == minimap2[index].mapq) {
            continue;
        }
        return {false, fmt::format(
            "diagnostic MAPQ vector differs at repeat={} ordered_index={} "
            "bioseq=(query_id={},mapq={}) minimap2=(query_id={},mapq={}) "
            "bioseq_mapq_sha256={} minimap2_mapq_sha256={}; numeric "
            "minimap2 MAPQ equivalence is not a validity requirement",
            repeat, index, bioseq[index].query_id, bioseq[index].mapq,
            minimap2[index].query_id, minimap2[index].mapq,
            bioseq_output.ordered_query_mapq_sha256,
            minimap2_output.ordered_query_mapq_sha256)};
    }
    return {true, fmt::format(
        "diagnostic MAPQ vectors happen to match at repeat={}; this does "
        "not claim minimap2 MAPQ semantics", repeat)};
}

/**
 * @brief Finalize and atomically seal one same-repeat Bioseq/minimap2 pair.
 *
 * The framework invokes this callback after both algorithm callback envelopes
 * have ended and before either output is evaluated or durably appended.  The
 * direct comparison therefore needs no shared state, lock, or vector copy.
 * A mismatch (or either failed logical row) mutates both outputs to INVALID,
 * so no first-row timing can remain independently eligible.
 */
void finalize_paired_minimap2_oracle_repeat(
        const e2e_input& in,
        const std::size_t repeat,
        std::span<bioseq::bench::framework::repeat_output_reference<e2e_output>>
            outputs) {
    if (!in.paired_minimap2_oracle) return;
    e2e_output* bioseq_output = nullptr;
    e2e_output* minimap2_output = nullptr;
    for (auto& reference : outputs) {
        if (reference.output == nullptr || reference.params == nullptr) {
            std::fputs(
                "[bench_e2e] PAIRED-ORACLE-FATAL null repeat output\n",
                stderr);
            std::fflush(stderr);
            std::abort();
        }
        if (reference.algorithm_name ==
            bioseq::bench::e2e::detail::algorithm_matrix_just_index) {
            if (bioseq_output != nullptr) {
                std::fputs(
                    "[bench_e2e] PAIRED-ORACLE-FATAL duplicate Bioseq row\n",
                    stderr);
                std::fflush(stderr);
                std::abort();
            }
            bioseq_output = reference.output;
        } else if (reference.algorithm_name == "minimap2_just_index") {
            if (minimap2_output != nullptr) {
                std::fputs(
                    "[bench_e2e] PAIRED-ORACLE-FATAL duplicate minimap2 row\n",
                    stderr);
                std::fflush(stderr);
                std::abort();
            }
            minimap2_output = reference.output;
        } else {
            std::fputs(
                "[bench_e2e] PAIRED-ORACLE-FATAL unexpected algorithm row\n",
                stderr);
            std::fflush(stderr);
            std::abort();
        }
    }
    if (outputs.size() != 2u || bioseq_output == nullptr
        || minimap2_output == nullptr) {
        std::fputs(
            "[bench_e2e] PAIRED-ORACLE-FATAL logical repeat does not contain "
            "exactly one Bioseq and one minimap2 row\n",
            stderr);
        std::fflush(stderr);
        std::abort();
    }

    finalize_ordered_query_oracle(in, *bioseq_output);
    finalize_ordered_query_oracle(in, *minimap2_output);
    const bool oracle_shape_valid =
        bioseq_output->ordered_query_oracle_available == 1
        && minimap2_output->ordered_query_oracle_available == 1
        && bioseq_output->ordered_query_oracle_sha256.size() == 64u
        && minimap2_output->ordered_query_oracle_sha256.size() == 64u
        && bioseq_output->ordered_query_mapq_sha256.size() == 64u
        && minimap2_output->ordered_query_mapq_sha256.size() == 64u;
    std::string diagnostic = oracle_shape_valid
        ? paired_oracle_mismatch_diagnostic(
            *bioseq_output, *minimap2_output, repeat)
        : fmt::format(
            "**INVALID**: paired minimap2 oracle repeat={} lacks a finalized "
            "ordered query oracle", repeat);
    const bool exact = diagnostic.empty();
    const auto [mapq_equal, mapq_diagnostic] =
        paired_mapq_diagnostic_comparison(
            *bioseq_output, *minimap2_output, repeat);
    for (e2e_output* output : {bioseq_output, minimap2_output}) {
        output->paired_oracle_repeat_ordinal =
            static_cast<std::int64_t>(repeat);
        output->paired_oracle_comparison_available = 1;
        output->paired_oracle_exact = exact ? 1 : 0;
        output->paired_oracle_diagnostic = diagnostic;
        output->paired_mapq_vector_equal = mapq_equal ? 1 : 0;
        output->paired_mapq_diagnostic = mapq_diagnostic;
        output->paired_oracle_verdict_sealed = 1;
        if (!mapq_equal) {
            if (!output->behavior_difference_comment.empty()) {
                output->behavior_difference_comment += "; ";
            }
            output->behavior_difference_comment += mapq_diagnostic;
            if (output->tuple_accepted == 1
                && output->cell_disposition == "accepted") {
                output->cell_disposition = "accepted_behavior_difference";
            }
        }
        if (!exact) {
            output->tuple_accepted = 0;
            output->phase = "paired_oracle";
            output->cell_disposition = "invalid";
            output->tuple_rejection_reason = diagnostic;
            output->behavior_difference_comment.clear();
        }
    }
    if (!exact) {
        std::fprintf(
            stderr, "[bench_e2e] PAIRED-ORACLE-INVALID %s\n",
            diagnostic.c_str());
        std::fflush(stderr);
    }
}

// HIGH-09/TESTING-04: the ground truth is known, so any miss or mis-map is ALWAYS
// a bug. Fail loudly rather than letting a fast, meaningless timing be reported
// as a "win". Called AFTER the timed region.
inline void require_mapped(std::string_view algo, const e2e_input& in,
                           const e2e_output& out) {
    if (mapped_result_is_valid(in, out)) return;
    const std::string offset_diagnostic = position_offset_diagnostic(out);
    bioseq_log<logging_level::error>("[bench_e2e] {} failed to map the query at {} bp (found={}, "
         "target_start={}, truth={}, offset={} bp >= {} threshold) — the timed result is "
         "invalid; aborting (match_count={}, expected={}, correct={}, "
         "correct_strand={}, correct_cigar={}, cigar_replay={}, cigar_projection={})",
         algo, in.target_bp, out.found, out.target_start,
         in.truth_target_start, offset_diagnostic, k_position_tolerance_bp,
         out.match_count, out.expected_match_count, out.correct_match_count,
         out.correct_strand_count, out.correct_cigar_count,
         out.cigar_replay_count, in.cigar_projection);
    // fmtlog is async; the ERR above can be lost when std::abort() runs before
    // the background poll flushes it. Emit a synchronous copy so the failing
    // cell is always visible in the captured stderr.
    std::fprintf(stderr,
        "[bench_e2e] MAP-FAIL algo=%.*s target_bp=%llu found=%d target_start=%lld "
        "truth=%lld offset_bp=%s tol=%lld match_count=%lld expected=%lld "
        "correct=%lld correct_strand=%lld correct_cigar=%lld cigar_replay=%lld "
        "cigar_projection=%d — aborting\n",
        static_cast<int>(algo.size()), algo.data(),
        static_cast<unsigned long long>(in.target_bp), static_cast<int>(out.found),
        static_cast<long long>(out.target_start),
        static_cast<long long>(in.truth_target_start),
        offset_diagnostic.c_str(), static_cast<long long>(k_position_tolerance_bp),
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

/** @brief Expected pipeline cardinality and worker shape for one statement. */
struct pipeline_stats_expectation {
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
            "[bench_e2e] PIPELINE-VALIDATION-EXPECTATION-OVERFLOW "
            "targets=%zu queries=%zu\n",
            target_rows,
            query_count);
        std::fflush(stderr);
        std::abort();
    }
    return static_cast<std::int64_t>(target_rows * query_count);
}

[[nodiscard]] pipeline_stats_expectation make_pipeline_stats_expectation(
        const e2e_input& in,
        const bool just_index,
        const int effective_cpu_workers) {
    // Both cold modes use exactly one physical target row and N independently
    // materialized query rows through BioseqBatchJoin. Physical target
    // duplication is not a supported benchmark topology.
    (void)just_index;
    constexpr std::size_t target_rows_per_dispatch = 1;
    const std::size_t queries_per_target_dispatch = in.queries.size();
    const bool headline_comparison_case = !in.paired_minimap2_oracle
        && (in.size_label.starts_with("2gbp_")
            || in.size_label.starts_with("4gbp_"));
    if (headline_comparison_case && queries_per_target_dispatch != 16) {
        std::fprintf(
            stderr,
            "[bench_e2e] PIPELINE-VALIDATION-SHAPE-FAIL headline case '%s' "
            "requires target_rows=1/query_rows=16; actual query_rows=%zu\n",
            in.size_label.c_str(),
            queries_per_target_dispatch);
        std::fflush(stderr);
        std::abort();
    }
    const std::int64_t expected_jobs = checked_pipeline_job_product(
        target_rows_per_dispatch,
        queries_per_target_dispatch);
    return pipeline_stats_expectation{
        .target_rows = static_cast<std::int64_t>(target_rows_per_dispatch),
        .attempted_alignment_jobs = expected_jobs,
        .target_rows_per_dispatch = static_cast<std::int64_t>(
            target_rows_per_dispatch),
        .effective_cpu_workers = effective_cpu_workers,
    };
}

[[nodiscard]] bool validate_alignment_stage_matrix(
        const e2e_output& out,
        const bioseq::pipeline_stats::executed_backend expected_backend,
        const bioseq::pipeline_stats::target_index_source expected_source) {
    using bioseq::pipeline_stats::execution_engine;
    using bioseq::pipeline_stats::pipeline_stage;
    using bioseq::pipeline_stats::engine_index;
    using bioseq::pipeline_stats::expected_execution_engine;
    using bioseq::pipeline_stats::is_front_end_eligibility_required;

    if (out.alignment_snapshot_rows.size()
            != e2e_output::k_alignment_snapshot_row_count
        || out.attempted_alignment_jobs <= 0) {
        return false;
    }
    const auto attempted = out.attempted_alignment_jobs;
    for (std::size_t row_index = 0;
         row_index < out.alignment_snapshot_rows.size(); ++row_index) {
        const auto& row = out.alignment_snapshot_rows[row_index];
        if (row.size() != e2e_output::k_alignment_snapshot_column_count) {
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
            static_cast<pipeline_stage>(row_index));
        if (expected_engine == execution_engine::count) {
            if (*eligible != 0
                || std::any_of(executed.begin(), executed.end(),
                    [](const std::int64_t count) { return count != 0; })
                || *gpu_device_ns != 0) {
                return false;
            }
            continue;
        }
        if (is_front_end_eligibility_required(
                expected_source,
                static_cast<pipeline_stage>(row_index))
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
        if (expected_engine != execution_engine::vulkan_gpu
            && *gpu_device_ns != 0) {
            return false;
        }
    }
    return true;
}

/** Resolve the stable GUC spelling into the runtime algorithm enum. */
[[nodiscard]] constexpr std::optional<::bioseq::simd::minimizer_algorithm>
e2e_minimizer_algorithm(std::string_view name) noexcept {
    using algorithm = ::bioseq::simd::minimizer_algorithm;
    if (name == "scalar") return algorithm::scalar;
    if (name == "simd_minimap2_reference") {
        return algorithm::simd_minimap2_reference;
    }
    if (name == "two_stack_nthash") return algorithm::two_stack_nthash;
    if (name == "vulkan_minimap_integer") {
        return algorithm::vulkan_minimap_integer;
    }
    if (name == "bounded_nthash_simd") {
        return algorithm::bounded_nthash_simd;
    }
    return std::nullopt;
}

/**
 * Resolve one E2E target through the generated runtime-profile registry.
 *
 * Alphabet wire ids and hash widths are source-owned by the generated table;
 * this helper deliberately has no DNA4 default or local k-to-width mapping.
 */
[[nodiscard]] const ::bioseq::minimizers::minimizer_runtime_profile_binding*
e2e_minimizer_runtime_profile(
        const e2e_input& in,
        const std::string_view requested_algorithm) noexcept {
    if (in.target_bp < 0 || in.kmer_size <= 0 || in.window_size <= 0 ||
        in.kmer_size > (std::numeric_limits<std::uint16_t>::max)() ||
        in.window_size > (std::numeric_limits<std::uint16_t>::max)()) {
        return nullptr;
    }
    const auto algorithm = e2e_minimizer_algorithm(requested_algorithm);
    const std::optional<std::uint16_t> alphabet_id = in.alphabet == "dna4"
        ? std::optional<std::uint16_t>{
              ::bioseq::minimizers::minimizer_profile_alphabet_id<seq::dna4>()}
        : in.alphabet == "dna5"
            ? std::optional<std::uint16_t>{
                  ::bioseq::minimizers::minimizer_profile_alphabet_id<seq::dna5>()}
            : std::nullopt;
    if (!algorithm || !alphabet_id) return nullptr;

    const auto runtime_k = static_cast<std::uint16_t>(in.kmer_size);
    const auto runtime_w = static_cast<std::uint16_t>(in.window_size);
    constexpr auto packed_storage_bits =
        static_cast<std::uint8_t>(sizeof(bench_storage_t) * 8u);
    // target_bp is a nonnegative int64 above, so the geometry helper's
    // greater-than-2^63 rejection cannot be reached here.
    const auto required_position_log2 =
        ::bioseq::minimizers::minimizer_position_requirement(
            static_cast<std::uint64_t>(in.target_bp));
    const auto* binding = ::bioseq::minimizers::find_minimizer_runtime_profile(
        *algorithm,
        *alphabet_id,
        runtime_k,
        runtime_w,
        required_position_log2,
        packed_storage_bits);
    if (binding == nullptr || binding->requested_algorithm != *algorithm ||
        binding->alphabet_id != *alphabet_id ||
        !::bioseq::minimizers::minimizer_runtime_profile_accepts_shape(
            *binding, runtime_k, runtime_w) ||
        binding->packed_storage_bits != packed_storage_bits ||
        required_position_log2 < binding->minimum_position_log2 ||
        required_position_log2 > binding->maximum_position_log2 ||
        ::bioseq::minimizers::find_minimizer_physical_record(binding->record_id) == nullptr) {
        return nullptr;
    }
    return binding;
}

[[nodiscard]] const ::bioseq::minimizers::minimizer_runtime_profile_binding&
require_e2e_minimizer_runtime_profile(
        const e2e_input& in,
        const std::string_view requested_algorithm) {
    const auto* binding = e2e_minimizer_runtime_profile(in, requested_algorithm);
    if (binding == nullptr) {
        abort_pipeline_stats_capture(fmt::format(
            "no generated E2E minimizer runtime profile for algorithm={} "
            "alphabet={} k={} w={} target_bp={} packed_storage_bits={}",
            requested_algorithm,
            in.alphabet,
            in.kmer_size,
            in.window_size,
            in.target_bp,
            sizeof(bench_storage_t) * 8u));
    }
    return *binding;
}

/** Select compact-H32 only when the generated MPF binding owns a u32 hash. */
[[nodiscard]] bool e2e_expects_compact_h32_direct(
        const e2e_input& in,
        const std::string_view requested_algorithm,
        const std::string_view minimizer_index) {
    return minimizer_index == k_minimizers_per_fragment_index &&
        require_e2e_minimizer_runtime_profile(in, requested_algorithm).hash_width ==
            ::bioseq::minimizers::minimizer_scalar_width::u32;
}

/**
 * Return the exact storage-stage engine for a canonical algorithm-matrix route.
 *
 * A selected Vulkan target sketch still feeds the existing host spill/merge
 * index handling for no-index execution.  Persisted MPF selection is likewise
 * a host traversal of the mapped source before its query-selected resident is
 * materialized.  Only persisted MPS performs partition selection against its
 * grouped-SoA device resident.  Both Vulkan seed layouts own a device-resident
 * handoff, including the transient no-index layouts.
 */
[[nodiscard]] constexpr bioseq::pipeline_stats::execution_engine
expected_algorithm_matrix_storage_engine(
        const bioseq::pipeline_stats::target_index_source source,
        const bioseq::pipeline_stats::pipeline_stage stage,
        const bool vulkan_seed,
        const bool mps_layout) noexcept {
    using bioseq::pipeline_stats::execution_engine;
    using bioseq::pipeline_stats::pipeline_stage;
    using bioseq::pipeline_stats::target_index_source;
    switch (stage) {
    case pipeline_stage::target_index_build:
        return source == target_index_source::recomputed
            ? execution_engine::scalar_cpu : execution_engine::count;
    case pipeline_stage::partition_selection:
        if (source != target_index_source::persisted)
            return execution_engine::count;
        return vulkan_seed && mps_layout
            ? execution_engine::vulkan_gpu : execution_engine::scalar_cpu;
    case pipeline_stage::device_index_upload:
        return vulkan_seed ? execution_engine::vulkan_gpu
                           : execution_engine::count;
    default:
        return bioseq::pipeline_stats::expected_execution_engine(
            bioseq::pipeline_stats::executed_backend::scalar_cpu,
            source,
            stage);
    }
}

static_assert(expected_algorithm_matrix_storage_engine(
    bioseq::pipeline_stats::target_index_source::recomputed,
    bioseq::pipeline_stats::pipeline_stage::target_index_build,
    true,
    false) == bioseq::pipeline_stats::execution_engine::scalar_cpu);
static_assert(expected_algorithm_matrix_storage_engine(
    bioseq::pipeline_stats::target_index_source::persisted,
    bioseq::pipeline_stats::pipeline_stage::partition_selection,
    true,
    false) == bioseq::pipeline_stats::execution_engine::scalar_cpu);
static_assert(expected_algorithm_matrix_storage_engine(
    bioseq::pipeline_stats::target_index_source::persisted,
    bioseq::pipeline_stats::pipeline_stage::partition_selection,
    true,
    true) == bioseq::pipeline_stats::execution_engine::vulkan_gpu);
static_assert(expected_algorithm_matrix_storage_engine(
    bioseq::pipeline_stats::target_index_source::recomputed,
    bioseq::pipeline_stats::pipeline_stage::device_index_upload,
    true,
    false) == bioseq::pipeline_stats::execution_engine::vulkan_gpu);
static_assert(expected_algorithm_matrix_storage_engine(
    bioseq::pipeline_stats::target_index_source::persisted,
    bioseq::pipeline_stats::pipeline_stage::device_index_upload,
    false,
    true) == bioseq::pipeline_stats::execution_engine::count);

[[nodiscard]] bool validate_algorithm_matrix_stage_paths(
        const e2e_output& out,
        bioseq::pipeline_stats::target_index_source source,
        const bool mps_layout) {
    using bioseq::pipeline_stats::execution_engine;
    using bioseq::pipeline_stats::pipeline_stage;
    using bioseq::pipeline_stats::engine_index;
    using bioseq::pipeline_stats::is_front_end_eligibility_required;
    const bool vulkan_seed =
        out.observed_minimizer_algorithm == "vulkan_minimap_integer";
    const bool vulkan_chain =
        out.observed_chaining_algorithm == "vulkan_tiled_dynamic_programming";
    if (out.alignment_snapshot_rows.size() != e2e_output::k_alignment_snapshot_row_count ||
        out.attempted_alignment_jobs <= 0) {
        return false;
    }
    for (std::size_t row_index = 0; row_index < out.alignment_snapshot_rows.size(); ++row_index) {
        const auto& row = out.alignment_snapshot_rows[row_index];
        const auto eligible = parse_snapshot_i64(row[24]);
        const auto fallback = parse_snapshot_i64(row[29]);
        const auto gpu_device_ns = parse_snapshot_i64(row[31]);
        if (!eligible.has_value() || !fallback.has_value() || !gpu_device_ns.has_value() ||
            *fallback != 0 || *gpu_device_ns < 0 || *eligible < 0 ||
            *eligible > out.attempted_alignment_jobs) {
            return false;
        }
        const auto stage = static_cast<pipeline_stage>(row_index);
        execution_engine expected = expected_algorithm_matrix_storage_engine(
            source, stage, vulkan_seed, mps_layout);
        if (expected != execution_engine::count &&
            (stage == pipeline_stage::query_sketch || stage == pipeline_stage::target_sketch)) {
            expected = vulkan_seed ? execution_engine::vulkan_gpu
                : out.observed_minimizer_algorithm == "scalar"
                ? execution_engine::scalar_cpu : execution_engine::simd_cpu;
        } else if (stage == pipeline_stage::seed_lookup) {
            expected = vulkan_seed ? execution_engine::vulkan_gpu : execution_engine::scalar_cpu;
        } else if (stage == pipeline_stage::chain_score ||
                   stage == pipeline_stage::chain_backtrack) {
            expected = vulkan_chain && stage == pipeline_stage::chain_score
                ? execution_engine::vulkan_gpu
                : out.observed_chaining_algorithm == "simd_minimap2"
                ? execution_engine::simd_cpu : execution_engine::scalar_cpu;
        } else if (stage == pipeline_stage::left_extension ||
                   stage == pipeline_stage::gap_extension ||
                   stage == pipeline_stage::right_extension) {
            expected = out.observed_base_alignment_algorithm == "block_aligner"
                ? execution_engine::simd_cpu : execution_engine::scalar_cpu;
        }
        if (expected != execution_engine::vulkan_gpu && *gpu_device_ns != 0)
            return false;
        const std::array<std::optional<std::int64_t>, 4> executed{
            parse_snapshot_i64(row[25]), parse_snapshot_i64(row[26]),
            parse_snapshot_i64(row[27]), parse_snapshot_i64(row[28]),
        };
        if (std::ranges::any_of(executed, [](const auto& value) { return !value.has_value(); })) {
            return false;
        }
        if (expected == execution_engine::count) {
            if (*eligible != 0 || std::ranges::any_of(
                    executed, [](const auto& value) { return *value != 0; })) {
                return false;
            }
            continue;
        }
        if (is_front_end_eligibility_required(source, stage) &&
            *eligible != out.attempted_alignment_jobs) {
            return false;
        }
        for (std::size_t engine = 0; engine < executed.size(); ++engine) {
            const std::int64_t expected_count =
                engine == engine_index(expected) ? *eligible : 0;
            if (*executed[engine] != expected_count) return false;
        }
    }
    return true;
}

[[nodiscard]] bool require_pipeline_execution_stats(
        std::string_view algo,
        const e2e_input& in,
        e2e_output& out,
        const pipeline_stats_expectation& expectation,
        std::string_view minimizer_index) {
    const bool expect_gpu =
        algo.find("_gpu_") != std::string_view::npos
        || algo.find("_vulkan_") != std::string_view::npos;
    const bool expect_simd =
        algo.find("_simd_") != std::string_view::npos;
    const bool expect_scalar =
        algo.find("_scalar_") != std::string_view::npos;
    const bool matrix_tuple =
        algo == bioseq::bench::e2e::detail::algorithm_matrix_just_index ||
        algo == bioseq::bench::e2e::detail::algorithm_matrix_no_index;
    if (!expect_gpu && !expect_simd && !expect_scalar && !matrix_tuple) {
        return true;
    }
    const bool expect_persisted =
        algo.ends_with("_just_index");
    const bool matrix_vulkan_seed = matrix_tuple &&
        out.requested_minimizer_algorithm == "vulkan_minimap_integer";
    // A matrix GPU MPF reader maps its source into a resident; u32 hashes do
    // not imply the CPU compact-direct reader's zero-directory contract.
    const bool expect_compact_h32_direct =
        expect_persisted && !matrix_vulkan_seed && e2e_expects_compact_h32_direct(
            in,
            matrix_tuple ? std::string_view{out.requested_minimizer_algorithm}
                         : std::string_view{out.attested_requested_minimizer_algorithm},
            minimizer_index);
    const bool matrix_vulkan_chain = matrix_tuple &&
        out.requested_chaining_algorithm == "vulkan_tiled_dynamic_programming";
    const bool matrix_hybrid_vulkan = matrix_vulkan_seed || matrix_vulkan_chain;
    const bool matrix_all_scalar = matrix_tuple &&
        out.requested_minimizer_algorithm == "scalar" &&
        out.requested_chaining_algorithm == "scalar_minimap2" &&
        out.requested_base_alignment_algorithm == "ksw2";
    const bool matrix_all_simd = matrix_tuple &&
        (out.requested_minimizer_algorithm == "two_stack_nthash" ||
         out.requested_minimizer_algorithm == "bounded_nthash_simd") &&
        out.requested_chaining_algorithm == "simd_minimap2" &&
        out.requested_base_alignment_algorithm == "block_aligner";
    const bool effective_expect_gpu = expect_gpu || matrix_hybrid_vulkan;
    const bool expect_gpu_suffix = expect_gpu && !matrix_tuple;
    const bool expect_gpu_seed = expect_gpu_suffix || matrix_vulkan_seed;
    // Canonical MPF persisted GPU lookup retains the mapped source plus one
    // query-selected resident; it does not prepare a CPU target index. MPS
    // consumes the complete grouped-SoA generation for both CPU and GPU
    // readers when hash-major is not the selected CPU layout. Keep the legacy
    // fixed-GPU reader expectation isolated from matrix-route classification.
    const bool expect_full_generation_reader = expect_gpu_suffix ||
        (expect_persisted &&
         minimizer_index == k_minimizers_per_sequence_index);
    const std::string_view expected_backend = matrix_hybrid_vulkan
        ? "hybrid_cpu_vulkan"
        : matrix_all_scalar ? "scalar_cpu"
        : matrix_all_simd ? "simd_cpu"
        : matrix_tuple ? "mixed_cpu"
        : expect_gpu
        ? "vulkan_gpu"
        : expect_simd
            ? "simd_cpu"
            : "scalar_cpu";
    const std::string_view expected_source =
        expect_persisted ? "persisted" : "recomputed";
    // Matrix tuples prove their heterogeneous engine choices stage-by-stage;
    // only fixed benchmark cases have one backend-enum expectation.
    const auto expected_fixed_backend_enum = expect_gpu
        ? bioseq::pipeline_stats::executed_backend::vulkan_gpu
        : expect_simd
            ? bioseq::pipeline_stats::executed_backend::simd_cpu
            : bioseq::pipeline_stats::executed_backend::scalar_cpu;
    const auto expected_source_enum = expect_persisted
        ? bioseq::pipeline_stats::target_index_source::persisted
        : bioseq::pipeline_stats::target_index_source::recomputed;
    if (matrix_tuple &&
        (out.requested_backend == "mixed_invalid" || out.actual_backend == "mixed_invalid")) {
        out.tuple_accepted = 0;
        out.cell_disposition = "rejected_attestation";
        out.phase = "attestation";
        out.tuple_rejection_reason = fmt::format(
            "production attestation requested_backend={} actual_backend={} declines={} fallbacks={}",
            out.requested_backend, out.actual_backend, out.backend_declines, out.backend_fallbacks);
        return false;
    }
    out.stage_engine_matrix_valid =
        (matrix_tuple
             ? validate_algorithm_matrix_stage_paths(
                   out,
                   expected_source_enum,
                   minimizer_index == k_minimizers_per_sequence_index)
             : validate_alignment_stage_matrix(
                   out, expected_fixed_backend_enum, expected_source_enum))
        ? 1 : 0;
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
    const auto counter = [&out](const std::size_t column) noexcept {
        return out.alignment_snapshot_numeric_columns[column];
    };
    const bool vulkan_host_cigar_receipts_coherent =
        !expect_gpu_suffix ||
        (out.host_query_symbol_packing_calls == 0 && out.host_target_symbol_packing_calls == 0 &&
         out.host_alignment_job_classification_calls == 0 &&
         out.host_cigar_construction_calls == 0);
    const bool vulkan_persisted_window_io_coherent =
        !expect_gpu_suffix || !in.cigar_projection || !expect_persisted ||
        (counter(118) > 0 && counter(119) > 0 && counter(120) > 0 && counter(121) == counter(120) &&
         counter(122) > 0);
    const bool backend_specific = matrix_hybrid_vulkan
        ? (matrix_vulkan_seed ? out.gpu_seed_dispatches > 0 : out.gpu_seed_dispatches == 0)
            && (matrix_vulkan_chain ? out.gpu_chain_dispatches > 0 : out.gpu_chain_dispatches == 0)
            && out.gpu_chain_backtrack_dispatches == 0
            && out.gpu_extend_dispatches == 0
            && counter(117) == 0
            && counter(118) == 0
        : expect_gpu_suffix
        ? out.gpu_seed_dispatches > 0
            && out.gpu_chain_dispatches > 0
            && out.gpu_chain_backtrack_dispatches > 0
            && (!in.cigar_projection || out.gpu_extend_dispatches > 0)
            && out.host_query_hash_readbacks == 0
            && vulkan_host_cigar_receipts_coherent
            && vulkan_persisted_window_io_coherent
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
        !expect_gpu_suffix || expectation.attempted_alignment_jobs >= 2;
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
    const bool expect_hash_major =
        expect_persisted && e2e_expects_hash_major(in, minimizer_index, expect_gpu_seed);
    const bool expect_grouped_soa =
        expect_persisted && !expect_hash_major;
    // MPS retains a query-selected position image after reading its complete
    // generation. Only MPF uses the zero-position-buffer fragment-native route.
    const bool expect_sequence_grouped_soa_reader =
        expect_grouped_soa && !expect_gpu_seed
        && minimizer_index == k_minimizers_per_sequence_index;
    const bool expect_fragment_native_reader =
        expect_grouped_soa && !expect_gpu_seed
        && minimizer_index == k_minimizers_per_fragment_index;
    out.persisted_layout = expect_hash_major
        ? "hash_major" : expect_grouped_soa ? "grouped_soa" : "none";
    const bool persisted_manifest_size_valid =
        scaled_equal(
            counter(56), counter(55),
            bioseq::indexing::minimizer_generation_manifest::
                encoded_bytes);
    out.persisted_format5_manifest_contract_valid = expect_grouped_soa
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
    const bool mapped_grouped_soa_cold_route =
        expect_grouped_soa
        && out.cold_index_route_empty == 0
        && out.cold_index_route_selective == 0
        && out.cold_index_route_full == 0;
    const bool one_cold_route =
        mapped_grouped_soa_cold_route
        || ((!expect_grouped_soa || expect_full_generation_reader)
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
    // An MPS grouped-SoA reader authenticates the directory and streams the
    // complete generation exactly once. MPF instead keeps its mapped image as
    // the source for either CPU lookup or query-selected GPU materialization.
    const bool grouped_soa_full_generation_read_gate =
        expect_grouped_soa
        && expect_full_generation_reader
        && out.cold_index_schema_valid == 1
        && full_read_gate
        && out.cold_index_route_candidate_rows == 0
        && out.cold_index_route_candidate_bytes == 0
        && out.cold_index_route_selective_cost == 0
        && out.cold_index_route_full_cost > 0;
    const bool grouped_soa_mapped_image_read_gate =
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
    const bool grouped_soa_route_gate = expect_full_generation_reader
        ? grouped_soa_full_generation_read_gate
        : grouped_soa_mapped_image_read_gate;
    const bool grouped_soa_metadata_read_shape =
        expect_full_generation_reader || !expect_compact_h32_direct
        ? counter(57) > 0 && counter(58) > 0
        : counter(57) == 0 && counter(58) == 0;
    const bool grouped_soa_mapped_payload_read_shape =
        counter(62) == 0
        && counter(63) == 0
        && std::all_of(
            out.alignment_snapshot_numeric_columns.begin() + 68,
            out.alignment_snapshot_numeric_columns.begin() + 82,
            [](const std::int64_t value) noexcept {
                return value == 0;
            });
    // These are the externally visible equations of
    // read_full_generation_payload_once(). Keep this stricter than merely
    // observing non-zero payload work: all requested extents/chunks must be
    // delivered in the one authenticated range scan with no skipped work.
    const bool grouped_soa_full_generation_payload_read_shape =
        counter(62) > 0
        && counter(62) == out.cold_index_payload_rows_delivered
        && counter(63) > 0
        && counter(63) == out.cold_index_payload_bytes_delivered
        && counter(68) > 0
        && counter(68) == counter(69)
        && counter(68) == counter(70)
        && counter(70) == counter(74)
        && counter(71) > 0
        && counter(71) == counter(62)
        && counter(71) == counter(75)
        && counter(72) == 0
        && counter(73) == 0
        && counter(76) == counter(63)
        && counter(77) == 1
        && counter(78) == 1
        && counter(79) == 1
        && counter(80) == 1
        && counter(81) == 0;
    const bool grouped_soa_payload_read_shape = expect_full_generation_reader
        ? grouped_soa_full_generation_payload_read_shape
        : grouped_soa_mapped_payload_read_shape;
    const bool grouped_soa_persisted_read_gate = expect_grouped_soa
        ? out.cold_index_schema_valid == 1
            && grouped_soa_route_gate
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
            && grouped_soa_metadata_read_shape
            && counter(59) == 0
            && static_cast<__int128>(counter(60))
                == static_cast<__int128>(counter(52))
                    + static_cast<__int128>(counter(55))
                    + static_cast<__int128>(counter(57))
            && static_cast<__int128>(counter(61))
                == static_cast<__int128>(counter(53))
                    + static_cast<__int128>(counter(56))
                    + static_cast<__int128>(counter(58))
            && grouped_soa_payload_read_shape
            && sum_equal(counter(64), counter(60), counter(62))
            && counter(67) == counter(61)
            && counter(67) > 0
            && static_cast<__int128>(out.persisted_index_scans)
                == static_cast<__int128>(counter(51))
                    + static_cast<__int128>(counter(54))
                    + static_cast<__int128>(counter(80))
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
    // failed cold-reader stats identify its exact class of disagreement
    // without another opaque E2E run.
    const bool persisted_read_route_stats =
        expect_grouped_soa
            ? grouped_soa_route_gate
                && cold_payload_equations
            : out.cold_index_schema_valid == 1
                && one_cold_route
                && (selective_read_gate || full_read_gate)
                && cold_payload_equations;
    const bool persisted_read_session_stats =
        out.persisted_generation_read_sessions == 1
        && out.persisted_generation_read_sessions == out.target_rows
        && out.persisted_relation_opens == out.target_rows;
    const bool persisted_read_head_stats =
        counter(51) == out.persisted_generation_read_sessions
        && counter(52) == out.persisted_generation_read_sessions
        && scaled_equal(counter(53), counter(52), 64);
    const bool persisted_read_manifest_stats =
        counter(54) == out.persisted_generation_read_sessions
        && counter(55) == out.persisted_generation_read_sessions
        && out.persisted_manifest_rows_read == counter(55)
        && out.persisted_manifest_bytes_read == counter(56)
        && out.persisted_format5_manifest_contract_valid == 1
        && persisted_manifest_size_valid;
    const bool persisted_read_directory_stats =
        grouped_soa_metadata_read_shape
        && counter(59) == 0
        && static_cast<__int128>(counter(60))
            == static_cast<__int128>(counter(52))
                + static_cast<__int128>(counter(55))
                + static_cast<__int128>(counter(57))
        && static_cast<__int128>(counter(61))
            == static_cast<__int128>(counter(53))
                + static_cast<__int128>(counter(56))
                + static_cast<__int128>(counter(58));
    const bool persisted_read_payload_stats =
        expect_grouped_soa
            ? grouped_soa_payload_read_shape
                && counter(67) == counter(61)
                && counter(67) > 0
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
    const bool persisted_read_zero_work_stats =
        counter(65) == 0
        && counter(66) == 0
        && counter(73) == 0
        && counter(82) == 0
        && counter(83) == 0
        && counter(84) == 0
        && counter(85) == 0;
    const bool no_minimizer_fragment_reader_stats =
        out.grouped_soa_reader_capacity_accounting_passes == 0
        && out.grouped_soa_reader_global_position_storage_preparations == 0
        && out.grouped_soa_reader_global_position_storage_allocations == 0
        && out.grouped_soa_reader_position_append_growth_reservations == 0
        && out.grouped_soa_reader_retained_position_count == 0
        && out.grouped_soa_reader_retained_position_capacity == 0
        && out.grouped_soa_reader_directory_capacity_bytes == 0
        && out.grouped_soa_reader_query_hash_capacity_bytes == 0
        && out.grouped_soa_reader_query_record_capacity_bytes == 0
        && out.grouped_soa_reader_ordinal_route_capacity_bytes == 0
        && out.grouped_soa_reader_selected_extent_capacity_bytes == 0
        && out.grouped_soa_reader_current_selected_control_capacity_bytes == 0
        && out.grouped_soa_reader_global_position_capacity_bytes == 0
        && out.grouped_soa_reader_control_payload_capacity_bytes == 0
        && out.grouped_soa_reader_owned_capacity_bytes == 0
        && out.grouped_soa_reader_admitted_bytes == 0
        && out.grouped_soa_reader_exact_position_image_checks == 0
        && out.grouped_soa_reader_exact_position_image_failures == 0
        && out.grouped_soa_reader_retained_layout_checks == 0
        && out.grouped_soa_reader_retained_layout_failures == 0
        && out.are_grouped_soa_reader_memory_stats_saturated == 0;
    const bool no_hash_major_index_reader_stats = [&out] {
        const auto& stats = out.hash_major_index_reader_stats;
        if (required_json_counter(stats, "format_revision") != 0
            || required_json_counter(stats, "sessions") != 0
            || required_json_counter(stats, "generation_id") != 0
            || required_json_text(stats, "source_identity")
                != "00000000000000000000000000000000"
            || required_json_text(stats, "source_content_crc64")
                != "0000000000000000"
            || required_json_text(stats, "compatibility_digest")
                != std::string(64, '0')
            || stats["is_saturated"].get<bool>()
            || stats["has_operational_identity_conflict"].get<bool>()
            || stats["are_stats_saturated"].get<bool>()) {
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
            "retained_position_count", "directory_stage_peak_bytes",
            "control_stage_peak_bytes", "posting_stage_peak_bytes",
            "peak_admitted_bytes", "retained_owner_bytes",
            "admitted_bytes", "assembly_calls",
            "pair_index_conversion_calls", "segment_rebase_calls",
            "generic_source_fallback_calls", "backend_fallbacks",
            "failures",
        });
        return std::ranges::all_of(zero_keys, [&stats](const auto key) {
            return required_json_counter(stats, key) == 0;
        });
    }();
    const bool hash_major_persisted_read_gate = !expect_hash_major || [&] {
        const auto& stats = out.hash_major_index_reader_stats;
        const auto& audit = out.post_restart_hash_major_ready_stats;
        const auto& source = required_json_object(audit, "source");
        const auto& head = required_json_object(audit, "head");
        const auto& manifest = required_json_object(audit, "manifest");
        const auto& compatibility =
            required_json_object(manifest, "compatibility");
        const auto& streams = required_json_array(audit, "streams");
        const auto hash_major = [&stats](const std::string_view key) {
            return required_json_counter(stats, key);
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
        const std::int64_t stage_peak = std::max({
            hash_major("directory_stage_peak_bytes"),
            hash_major("control_stage_peak_bytes"),
            hash_major("posting_stage_peak_bytes"),
        });
        const bool physical_metadata =
            out.persisted_generation_read_sessions == 1
            && out.persisted_generation_read_sessions == out.target_rows
            && out.persisted_relation_opens == out.target_rows
            && counter(51) == 1
            && counter(52) == 1
            && counter(53)
                == bioseq::indexing::k_hash_major_minimizer_head_bytes
            && counter(54) == 1
            && counter(55) == 1
            && counter(56)
                == bioseq::indexing::k_hash_major_minimizer_manifest_bytes
            && counter(142) == 1
            && counter(143) == 1
            && counter(144) == 1
            && counter(145) == 0
            && counter(146) == 0
            && counter(147) == 0
            && counter(148) == 0
            && counter(82) == 0;
        const bool runtime_identity =
            hash_major("format_revision") == 6
            && hash_major("sessions") == 1
            && hash_major("generation_id")
                == audit_counter(head, "generation_id")
            && hash_major("generation_id") == out.persisted_generation_id
            && required_json_text(stats, "source_identity")
                == required_json_text(source, "source_identity")
            && required_json_text(stats, "source_content_crc64")
                == required_json_text(source, "source_content_crc64")
            && required_json_text(stats, "compatibility_digest")
                == required_json_text(head, "compatibility_digest");
        const bool directory_receipts =
            hash_major("directory_rows") == directory_rows
            && hash_major("directory_bytes") == directory_bytes
            && hash_major("directory_row_crc_receipts") == directory_rows
            && hash_major("directory_stream_crc_receipts") == 1;
        const bool control_receipts =
            hash_major("control_rows") > 0
            && hash_major("control_bytes") > 0
            && hash_major("control_bytes") >= hash_major("control_rows")
            && static_cast<__int128>(hash_major("control_bytes"))
                <= static_cast<__int128>(hash_major("control_rows"))
                    * static_cast<__int128>(
                        bioseq::indexing::
                            k_hash_major_minimizer_row_payload_bytes)
            && hash_major("control_row_crc_receipts")
                == hash_major("control_rows")
            && hash_major("selected_block_count") > 0
            && hash_major("control_block_crc_receipts")
                == hash_major("selected_block_count")
            && hash_major("control_semantic_receipts")
                == hash_major("selected_block_count");
        const bool posting_receipts =
            hash_major("posting_rows") > 0
            && hash_major("posting_bytes") > 0
            && hash_major("posting_bytes") >= hash_major("posting_rows")
            && static_cast<__int128>(hash_major("posting_bytes"))
                <= static_cast<__int128>(hash_major("posting_rows"))
                    * static_cast<__int128>(
                        bioseq::indexing::
                            k_hash_major_minimizer_row_payload_bytes)
            && hash_major("posting_row_crc_receipts")
                == hash_major("posting_rows")
            && hash_major("posting_ranges_delivered") > 0
            && hash_major("posting_semantic_receipts")
                == hash_major("posting_ranges_delivered")
            && hash_major("posting_range_crc_receipts")
                <= hash_major("posting_ranges_delivered")
            && hash_major("retained_position_count") > 0
            && static_cast<__int128>(hash_major("authorized_posting_bytes"))
                == static_cast<__int128>(
                    hash_major("retained_position_count"))
                    * static_cast<__int128>(posting_width);
        const bool selective_work =
            hash_major("query_count") > 0
            && hash_major("accepted_hash_count") > 0
            && hash_major("accepted_hash_count")
                <= hash_major("query_count")
            && hash_major("selected_block_count")
                <= audit_counter(manifest, "directory_entry_count");
        const bool exact_memory =
            hash_major("directory_stage_peak_bytes") > 0
            && hash_major("control_stage_peak_bytes") > 0
            && hash_major("posting_stage_peak_bytes") > 0
            && hash_major("peak_admitted_bytes") == stage_peak
            && hash_major("retained_owner_bytes") > 0
            && hash_major("retained_owner_bytes")
                == hash_major("admitted_bytes");
        const bool forbidden_routes =
            hash_major("assembly_calls") == 0
            && hash_major("pair_index_conversion_calls") == 0
            && hash_major("segment_rebase_calls") == 0
            && hash_major("recompute_calls") == 0
            && hash_major("generic_source_fallback_calls") == 0
            && hash_major("backend_fallbacks") == 0
            && hash_major("failures") == 0
            && !stats["is_saturated"].get<bool>()
            && !stats["has_operational_identity_conflict"].get<bool>()
            && !stats["are_stats_saturated"].get<bool>();
        return physical_metadata && runtime_identity
            && directory_receipts && control_receipts
            && posting_receipts && selective_work && exact_memory
            && forbidden_routes;
    }();
    out.persisted_format6_contract_valid =
        expect_hash_major && hash_major_persisted_read_gate ? 1 : 0;
    const bool persisted_read_gate = expect_hash_major
        ? hash_major_persisted_read_gate
        : expect_grouped_soa
            ? grouped_soa_persisted_read_gate && no_hash_major_index_reader_stats
            : no_persisted_read_gate && no_hash_major_index_reader_stats;
    const __int128 grouped_soa_reader_sessions =
        static_cast<__int128>(out.persisted_generation_read_sessions);
    const __int128 grouped_soa_reader_passes = static_cast<__int128>(
        out.grouped_soa_reader_capacity_accounting_passes);
    /* grouped-SoA performs one fixed accounting pass. The compact-direct-only route
     * owns the nested direct mapping authority but no grouped-SoA directory,
     * payload, entry plan, or block plan. */
    const bool grouped_soa_reader_accounting_shape =
        grouped_soa_reader_sessions > 0
        && grouped_soa_reader_passes == grouped_soa_reader_sessions;
    const __int128 grouped_soa_reader_exact_position_bytes =
        static_cast<__int128>(out.grouped_soa_reader_retained_position_count)
            * static_cast<__int128>(sizeof(std::uint64_t));
    const __int128 grouped_soa_reader_layout_bytes =
        static_cast<__int128>(out.grouped_soa_reader_directory_capacity_bytes)
        + static_cast<__int128>(out.grouped_soa_reader_query_hash_capacity_bytes)
        + static_cast<__int128>(out.grouped_soa_reader_query_record_capacity_bytes)
        + static_cast<__int128>(out.grouped_soa_reader_ordinal_route_capacity_bytes)
        + static_cast<__int128>(out.grouped_soa_reader_selected_extent_capacity_bytes)
        + static_cast<__int128>(
            out.grouped_soa_reader_current_selected_control_capacity_bytes)
        + static_cast<__int128>(
            out.grouped_soa_reader_global_position_capacity_bytes)
        + static_cast<__int128>(
            out.grouped_soa_reader_control_payload_capacity_bytes);
    const auto fragment_native = [&out](const std::string_view key) {
        return required_json_counter(
            out.minimizer_fragment_reader_stats, key);
    };
    const bool no_fragment_native_query_stats =
        std::ranges::all_of(
            k_fragment_native_stats_counter_keys,
            [&fragment_native](const std::string_view key) {
                return fragment_native(key) == 0;
            })
        && !out.minimizer_fragment_reader_stats[
            "are_fragment_native_stats_saturated"].get<bool>();
    const bool canonical_p3_packed_single_wave =
        in.size_label != "2gbp_dna4_grouped_soa_fragment_local_focus"
        || minimizer_index != k_minimizers_per_fragment_index
        || (fragment_native("fragment_native_waves") == 1
            && fragment_native("fragment_native_queries") == 16
            && fragment_native(
                    "fragment_native_packed_occurrence_waves") == 1
            && fragment_native(
                    "fragment_native_wide_occurrence_waves") == 0);
    // Per-wave shards are min(selected hashes, 32). The published counters
    // sum waves, so do not multiply two independently aggregated totals.
    const __int128 compact_direct_waves =
        fragment_native("fragment_native_waves");
    const __int128 compact_direct_queries =
        fragment_native("fragment_native_queries");
    const __int128 compact_direct_shards = fragment_native(
        "fragment_native_compact_direct_selection_shards");
    const __int128 compact_direct_query_layouts = fragment_native(
        "fragment_native_compact_direct_shard_query_layouts");
    const bool compact_direct_one_decode_gate = expect_compact_h32_direct
        ? compact_direct_waves > 0
            && compact_direct_waves <= compact_direct_queries
            && compact_direct_shards >= compact_direct_waves
            && compact_direct_shards <= 32 * compact_direct_waves
            && compact_direct_query_layouts >= compact_direct_shards
            && compact_direct_query_layouts >= compact_direct_queries
            && compact_direct_query_layouts <= 32 * compact_direct_queries
            && (compact_direct_waves != compact_direct_queries
                || compact_direct_query_layouts == compact_direct_shards)
            && (compact_direct_waves != 1
                || compact_direct_query_layouts
                    == compact_direct_shards * compact_direct_queries)
            && fragment_native(
                    "fragment_native_compact_direct_layout_shards_completed")
                == fragment_native(
                    "fragment_native_compact_direct_selection_shards")
            && fragment_native(
                    "fragment_native_compact_direct_emission_shards_completed")
                == fragment_native(
                    "fragment_native_compact_direct_selection_shards")
            && fragment_native(
                    "fragment_native_compact_direct_posting_visit_passes")
                == compact_direct_waves
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
            k_compact_direct_reader_stats_counter_keys,
            [&fragment_native](const std::string_view key) {
                return fragment_native(key) == 0;
            });
    const bool fragment_native_query_gate = expect_fragment_native_reader
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
                    * static_cast<__int128>(sizeof(bioseq::chaining::anchor))
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
            && (expect_compact_h32_direct
                ? fragment_native("fragment_native_query_plan_allocations") > 0
                    && static_cast<__int128>(fragment_native(
                        "fragment_native_query_plan_allocations"))
                        <= compact_direct_waves
                : fragment_native("fragment_native_query_plan_allocations") == 0)
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
                    "fragment_native_peak_block_stats_capacity_bytes") > 0
            && !out.minimizer_fragment_reader_stats[
                "are_fragment_native_stats_saturated"].get<bool>()
        : no_fragment_native_query_stats;
    // These are the existing MPS reader's four allocation-ledger barriers:
    // query routing, end of controls, position allocation, and finish. Keep
    // its exact-sized image and admission checks, not MPF's zero-buffer shape.
    const bool sequence_grouped_soa_reader_stats_gate =
        expect_sequence_grouped_soa_reader
        && grouped_soa_reader_sessions > 0
        && grouped_soa_reader_passes == 4 * grouped_soa_reader_sessions
        && out.grouped_soa_reader_global_position_storage_preparations
            == out.persisted_generation_read_sessions
        && out.grouped_soa_reader_global_position_storage_allocations
            == out.persisted_generation_read_sessions
        && out.grouped_soa_reader_position_append_growth_reservations == 0
        && out.grouped_soa_reader_retained_position_count > 0
        && out.grouped_soa_reader_retained_position_capacity
            == out.grouped_soa_reader_retained_position_count
        && grouped_soa_reader_exact_position_bytes
            == out.grouped_soa_reader_global_position_capacity_bytes
        && out.grouped_soa_reader_exact_position_image_checks
            == out.persisted_generation_read_sessions
        && out.grouped_soa_reader_exact_position_image_failures == 0
        && out.grouped_soa_reader_retained_layout_checks
            == out.persisted_generation_read_sessions
        && out.grouped_soa_reader_retained_layout_failures == 0
        && grouped_soa_reader_layout_bytes
            == out.grouped_soa_reader_owned_capacity_bytes
        && out.grouped_soa_reader_owned_capacity_bytes
            == out.grouped_soa_reader_admitted_bytes
        && out.grouped_soa_reader_directory_capacity_bytes > 0
        && out.grouped_soa_reader_query_hash_capacity_bytes > 0
        && out.grouped_soa_reader_current_selected_control_capacity_bytes == 0
        && out.grouped_soa_reader_control_payload_capacity_bytes == 0
        && no_fragment_native_query_stats
        && out.are_grouped_soa_reader_memory_stats_saturated == 0;
    const bool minimizer_fragment_reader_stats_gate =
        expect_sequence_grouped_soa_reader
        ? sequence_grouped_soa_reader_stats_gate
        : expect_fragment_native_reader
        ? out.grouped_soa_reader_global_position_storage_preparations == 0
            && out.grouped_soa_reader_global_position_storage_allocations == 0
            && out.grouped_soa_reader_position_append_growth_reservations == 0
            && out.grouped_soa_reader_retained_position_count == 0
            && out.grouped_soa_reader_retained_position_capacity == 0
            && out.grouped_soa_reader_global_position_capacity_bytes == 0
            && grouped_soa_reader_exact_position_bytes == 0
            && out.grouped_soa_reader_exact_position_image_checks
                == out.persisted_generation_read_sessions
            && out.grouped_soa_reader_exact_position_image_failures == 0
            && out.grouped_soa_reader_retained_layout_checks
                == out.persisted_generation_read_sessions
            && out.grouped_soa_reader_retained_layout_failures == 0
            && grouped_soa_reader_layout_bytes
                == static_cast<__int128>(
                    out.grouped_soa_reader_owned_capacity_bytes)
            && out.grouped_soa_reader_owned_capacity_bytes
                == out.grouped_soa_reader_admitted_bytes
            && (expect_compact_h32_direct
                ? out.grouped_soa_reader_directory_capacity_bytes == 0
                : out.grouped_soa_reader_directory_capacity_bytes > 0)
            && out.grouped_soa_reader_query_record_capacity_bytes == 0
            && (expect_compact_h32_direct
                ? out.grouped_soa_reader_ordinal_route_capacity_bytes == 0
                : out.grouped_soa_reader_ordinal_route_capacity_bytes > 0)
            && out.grouped_soa_reader_selected_extent_capacity_bytes == 0
            && out.grouped_soa_reader_query_hash_capacity_bytes == 0
            && out.grouped_soa_reader_current_selected_control_capacity_bytes == 0
            && out.grouped_soa_reader_control_payload_capacity_bytes == 0
            && grouped_soa_reader_accounting_shape
            && fragment_native_query_gate
            && out.are_grouped_soa_reader_memory_stats_saturated == 0
        : no_minimizer_fragment_reader_stats
            && fragment_native_query_gate;
    // CPU routes prepare one canonical query-key set. Persisted readers retain
    // that set without constructing a hashmap; no-index routes build each
    // physical parameter group once and reuse it for the remaining jobs. A
    // selected Vulkan frontend owns neither CPU preparation shape.
    const bool cpu_prepared_physical_gate =
        expect_gpu_seed || (!matrix_tuple && !expect_persisted)
        ? out.cpu_prepared_index_builds == 0
            && out.cpu_prepared_index_reuses == 0
        : expect_persisted
        ? out.cpu_prepared_index_builds == 0
            && out.cpu_prepared_index_reuses == 0
        : out.cpu_prepared_index_builds > 0
            && out.cpu_prepared_index_builds
                <= expectation.attempted_alignment_jobs
            && static_cast<__int128>(out.cpu_prepared_index_builds)
                    + static_cast<__int128>(out.cpu_prepared_index_reuses)
                == static_cast<__int128>(
                    expectation.attempted_alignment_jobs);
    const bool cpu_prepared_identity_gate =
        !expect_gpu_seed && (matrix_tuple || expect_persisted)
        ? out.cpu_prepared_unique_keys > 0
            // Compact-direct query plans are occurrence groups, whereas CPU
            // preparation records canonical unique query keys.
            && fixed_lower_hex(out.cpu_prepared_key_set_checksum, 16)
            && fixed_lower_hex(out.cpu_prepared_build_set_checksum, 16)
            && out.cpu_prepared_key_set_checksum
                == out.cpu_prepared_build_set_checksum
        : out.cpu_prepared_unique_keys == 0
            && out.cpu_prepared_key_set_checksum.empty()
            && out.cpu_prepared_build_set_checksum.empty();
    const bool cpu_prepared_gate = cpu_prepared_physical_gate
        && cpu_prepared_identity_gate
        && out.cpu_prepared_duplicate_build_attempts == 0;
    // Matrix statement-owned residents are materialized once and then borrowed.
    // Keep the separate legacy fixed-GPU route's per-job cache accounting.
    const bool gpu_residency_gate = expect_persisted && expect_gpu_seed
        ? out.gpu_persisted_resident_uploads == 1
            && out.gpu_persisted_resident_cache_hits ==
                (matrix_tuple ? 0 : expectation.attempted_alignment_jobs - 1)
        : out.gpu_persisted_resident_uploads == 0
            && out.gpu_persisted_resident_cache_hits == 0;
    const bool simd_json_counters_match_prefix =
        out.simd_stats_schema_valid == 1
        && out.nthash_vector_batches == counter(94);
    const bool no_block_aligner_execution_stats =
        out.requested_simd_alignment_algorithm == "none"
        && out.observed_simd_alignment_algorithm == "none"
        && out.block_aligner_execution_stats_available == 0
        && out.block_aligner_execution_stats_consistent == 0
        && out.block_aligner_executed_calls == 0
        && out.block_aligner_nonempty_calls == 0
        && out.block_aligner_nonempty_calls_with_simd == 0
        && out.block_aligner_simd_dp_vector_batches == 0
        && out.block_aligner_simd_dp_cells == 0
        && out.block_aligner_forbidden_ksw2_calls == 0
        && out.block_aligner_forbidden_scalar_biological_fallbacks == 0
        && out.block_aligner_execution_stats_saturated == 0;
    const bool accepted_block_aligner_execution_stats =
        out.requested_simd_alignment_algorithm == "block_aligner"
        && out.observed_simd_alignment_algorithm == "block_aligner"
        && out.block_aligner_execution_stats_available == 1
        && out.block_aligner_execution_stats_consistent == 1
        && out.block_aligner_executed_calls > 0
        && out.block_aligner_nonempty_calls > 0
        && out.block_aligner_nonempty_calls_with_simd
            == out.block_aligner_nonempty_calls
        && out.block_aligner_simd_dp_vector_batches > 0
        && out.block_aligner_simd_dp_cells > 0
        && out.block_aligner_forbidden_ksw2_calls == 0
        && out.block_aligner_forbidden_scalar_biological_fallbacks == 0
        && out.block_aligner_execution_stats_saturated == 0;
    const bool no_simd_policy_stats =
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
        && out.simd_chaining_policy_stats_saturated == 0
        && no_block_aligner_execution_stats;
    const bool bounded_simd_policy_stats =
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
        && out.simd_chaining_policy_stats_saturated == 0
        && accepted_block_aligner_execution_stats;
    // Algorithm identities are existing fields. Validate their component work
    // independently: a GPU seed may feed SIMD DP, and GPU DP may feed Block.
    const bool matrix_nthash_selected =
        out.observed_minimizer_algorithm == "two_stack_nthash" ||
        out.observed_minimizer_algorithm == "bounded_nthash_simd";
    const bool matrix_simd_chain_selected =
        out.observed_chaining_algorithm == "simd_minimap2";
    const bool matrix_component_work_gate = !matrix_tuple ||
        (simd_json_counters_match_prefix
         && (matrix_nthash_selected
             ? out.nthash_vector_batches > 0 || out.nthash_resident_vector_stream_kernel_calls > 0
             : out.nthash_vector_batches == 0 && out.nthash_resident_vector_stream_kernel_calls == 0)
         && out.simd_chaining_exact_policy_calls == 0
         && out.simd_chaining_policy_rejections == 0
         && out.simd_chaining_policy_stats_saturated == 0
         && (matrix_simd_chain_selected
             ? out.requested_simd_chaining_policy == "bounded_batch_approximate"
                 && out.observed_simd_chaining_policy == "bounded_batch_approximate"
                 && out.simd_chaining_bounded_policy_calls > 0
                 && out.simd_chaining_bounded_score_batches > 0
                 && counter(98) > 0 && counter(99) > 0
             : out.requested_simd_chaining_policy == "none"
                 && out.observed_simd_chaining_policy == "none"
                 && out.simd_chaining_bounded_policy_calls == 0
                 && out.simd_chaining_bounded_score_batches == 0
                 && out.simd_chaining_bounded_cutoff_stops == 0
                 && out.simd_chaining_bounded_candidates_skipped == 0
                 && counter(98) == 0 && counter(99) == 0)
         && (out.observed_base_alignment_algorithm == "block_aligner"
             ? accepted_block_aligner_execution_stats : no_block_aligner_execution_stats)
         && counter(93) == 0 && counter(95) == 0
         && counter(100) == 0 && counter(101) == 0 && counter(102) == 0);
    // The canonical no-index Vulkan producer is attested by target-sketch and
    // resident receipts. It does not execute the retired whole-GPU radix/hash
    // table builder represented by these legacy counters.
    const bool matrix_legacy_gpu_index_build_gate = !matrix_tuple ||
        std::all_of(
            out.alignment_snapshot_numeric_columns.begin() + 105,
            out.alignment_snapshot_numeric_columns.begin() + 110,
            [](const std::int64_t value) noexcept { return value == 0; });
    const bool simd_operational_gate = !expect_simd
        || (bounded_simd_policy_stats
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
        || (no_simd_policy_stats
            && std::all_of(
                out.alignment_snapshot_numeric_columns.begin() + 94,
                out.alignment_snapshot_numeric_columns.begin() + 102,
                [](const std::int64_t value) noexcept { return value == 0; })
            && std::all_of(
                out.alignment_snapshot_numeric_columns.begin() + 103,
                out.alignment_snapshot_numeric_columns.begin() + 139,
                [](const std::int64_t value) noexcept { return value == 0; }));
    const bool gpu_cache_identity_gate = !effective_expect_gpu
        || gpu_cache_identity_stats_valid({
            .device_uuid = out.gpu_device_uuid,
            .driver = out.gpu_driver,
            .policy = out.gpu_pipeline_cache_policy,
            .seed_sha256 = out.gpu_pipeline_cache_seed_sha256,
            .seed_size = out.gpu_pipeline_cache_seed_size,
            .manifest_checksum = out.gpu_pipeline_cache_manifest_checksum,
            .hit = out.gpu_pipeline_cache_hit,
        });
    const bool gpu_cache_runtime_gate = !effective_expect_gpu
        || gpu_cache_runtime_stats_valid({
            .declared_seed_size = out.gpu_pipeline_cache_seed_size,
            .loads = counter(124),
            .seed_bytes = counter(125),
            .load_ns = counter(126),
            .creation_ns = counter(127),
            .timestamp_failures = counter(133),
            .pipeline_creates = counter(138),
            .backend_declines = out.backend_declines,
            .backend_fallbacks = out.backend_fallbacks,
        }, out.gpu_pipeline_cache_policy);
    const bool gpu_operational_gate = !effective_expect_gpu
        || (gpu_cache_identity_gate
            && gpu_cache_runtime_gate
            && (matrix_tuple
                ? matrix_component_work_gate
                    && counter(117) == 0 && counter(118) == 0
                    && (matrix_vulkan_seed || std::all_of(
                        out.alignment_snapshot_numeric_columns.begin() + 103,
                        out.alignment_snapshot_numeric_columns.begin() + 114,
                        [](const std::int64_t value) noexcept { return value == 0; }))
                : no_simd_policy_stats
            && counter(82) == 0
            && counter(84) == 0
            && counter(85) == 0
            && counter(86) == 0
            && counter(87) == 0
            && counter(91) == 0
            && vulkan_host_cigar_receipts_coherent
            && counter(93) == 0
            && std::all_of(
                out.alignment_snapshot_numeric_columns.begin() + 94,
                out.alignment_snapshot_numeric_columns.begin() + 103,
                [](const std::int64_t value) noexcept { return value == 0; })
            && counter(112) == 0));
    // The legacy ledger describes only the pure vulkan_gpu persisted
    // coordinate pipeline. Matrix Vulkan stages deliberately retain the
    // hybrid_cpu_vulkan backend and must prove that this ledger stayed empty;
    // their physical GPU work remains mandatory in the independent cache,
    // dispatch, residency, stage-engine, and component-work gates above.
    const bool persisted_gpu_operation_gate = !expect_persisted
        || (matrix_hybrid_vulkan
            ? out.has_exact_empty_persisted_gpu_operation_ledger()
            : !expect_gpu_seed
                || out.has_accepted_legacy_persisted_gpu_operation_ledger(
                    expectation.attempted_alignment_jobs));
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
    const bool grouped_soa_cold_restart_identity_gate = !expect_grouped_soa
        || (out.pre_restart_persisted_layout == "grouped_soa"
            && out.post_restart_persisted_layout == "grouped_soa"
            && fixed_lower_hex(
                out.pre_restart_persisted_source_identity, 16)
            && out.pre_restart_persisted_source_identity
                != "0000000000000000"
            && out.pre_restart_persisted_source_identity
                == out.persisted_source_identity);
    const bool hash_major_cold_restart_identity_gate = !expect_hash_major
        || (out.pre_restart_persisted_layout == "hash_major"
            && out.post_restart_persisted_layout == "hash_major"
            && out.format6_restart_audit_accepted == 1
            && out.pre_restart_hash_major_ready_stats
                == out.post_restart_hash_major_ready_stats
            && fixed_lower_hex(
                out.pre_restart_persisted_source_identity, 32)
            && required_json_text(
                out.hash_major_index_reader_stats, "source_identity")
                == out.pre_restart_persisted_source_identity);
    const bool cold_restart_gate = cold_restart_process_gate
        && grouped_soa_cold_restart_identity_gate
        && hash_major_cold_restart_identity_gate;
    const bool cold_no_index_gate = expect_persisted
        || out.cold_no_index_lifecycle_accepted == 1;
    const bool persisted_policy_gate = !expect_persisted
        || out.persisted_policy_audit_accepted == 1;
    const bool stats_envelope_gate =
        out.pipeline_stats_available == 1
        && out.pipeline_stats_attached_to_measurement == 1
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
        && out.prefilter_execution_stats_available == 0
        && out.prefilter_activity_count == 0
        && out.prefilter_execution_stats_saturated == 0
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
    const bool documented_minimizer_fallback =
        out.requested_minimizer_algorithm == "simd_minimap2_reference"
        && out.observed_minimizer_algorithm == "scalar";
    const std::string_view expected_attested_base_alignment =
        in.cigar_projection ? std::string_view{out.requested_base_alignment_algorithm}
                            : std::string_view{"none"};
    const std::string_view expected_observed_base_alignment =
        !in.cigar_projection
            ? std::string_view{"none"}
            : expect_gpu_suffix
                ? std::string_view{"vulkan_simple_cigar"}
                : std::string_view{out.requested_base_alignment_algorithm};
    const bool algorithm_identity_gate =
        !matrix_tuple ||
        (out.attested_requested_minimizer_algorithm == out.requested_minimizer_algorithm &&
         out.attested_requested_chaining_algorithm == out.requested_chaining_algorithm &&
         (out.observed_minimizer_algorithm == out.requested_minimizer_algorithm ||
          documented_minimizer_fallback) &&
         out.observed_chaining_algorithm == out.requested_chaining_algorithm &&
         out.attested_requested_base_alignment_algorithm == expected_attested_base_alignment &&
         out.observed_base_alignment_algorithm == expected_observed_base_alignment &&
         out.observed_minimizer_algorithm != "none" &&
         out.observed_minimizer_algorithm != "mixed_invalid" &&
         out.observed_chaining_algorithm != "none" &&
         out.observed_chaining_algorithm != "mixed_invalid" &&
         out.attested_requested_base_alignment_algorithm != "mixed_invalid" &&
         out.observed_base_alignment_algorithm != "mixed_invalid");
    const bool mapq_execution_gate =
        out.mapq_execution_available == 1 && out.mapq_execution_conflict == 0 &&
        (!in.mapq_projection || out.mapq_executed == 1);
    const bool downstream_timing_gate =
        out.measured_region_available && out.downstream_active_wall_available == 1 &&
        out.downstream_active_wall_invocation_count == 1 &&
        out.downstream_active_wall_unavailable_invocation_count == 0 &&
        out.downstream_active_wall_ns >= 0 &&
        static_cast<std::uint64_t>(out.downstream_active_wall_ns) <= out.measured_region_ns;
    const auto interval_origins_cover_every_query = [&out, &in](const std::string_view kind,
                                                                const bool expected) {
        std::vector<bool> observed(in.queries.size(), false);
        bool any = false;
        for (const nlohmann::json& origin : out.benchmark_e2e_interval_origins) {
            if (origin["kind"].get_ref<const std::string&>() != kind)
                continue;
            any = true;
            const std::int64_t ordinal = required_json_counter(origin, "query_ordinal");
            if (!expected || ordinal < 0 ||
                static_cast<std::uint64_t>(ordinal) >= observed.size() ||
                observed[static_cast<std::size_t>(ordinal)]) {
                return false;
            }
            observed[static_cast<std::size_t>(ordinal)] = true;
        }
        return expected ? any && std::ranges::all_of(
                                     observed, [](const bool value) noexcept { return value; })
                        : !any;
    };
    const bool expect_cigar_interval =
        in.cigar_projection && out.observed_base_alignment_algorithm != "none";
    const bool expect_mapq_interval = in.mapq_projection && out.mapq_executed == 1;
    std::optional<std::int64_t> campaign_logical_target_row;
    bool campaign_origin_rows_coherent = true;
    for (const nlohmann::json& origin : out.benchmark_e2e_interval_origins) {
        const std::int64_t logical_target_row = required_json_counter(origin, "logical_target_row");
        if (campaign_logical_target_row.has_value() &&
            *campaign_logical_target_row != logical_target_row) {
            campaign_origin_rows_coherent = false;
            break;
        }
        campaign_logical_target_row = logical_target_row;
    }
    const std::size_t expected_origin_summary_count =
        in.queries.size() * (static_cast<std::size_t>(expect_cigar_interval) +
                             static_cast<std::size_t>(expect_mapq_interval));
    const bool benchmark_interval_timing_gate =
        out.measured_region_available
        && out.benchmark_e2e_interval_schema_revision == 6
        && out.benchmark_e2e_interval_available == 1
        && out.benchmark_e2e_interval_finalization_count == 1
        && out.benchmark_e2e_interval_failure_code == "none"
        && out.benchmark_e2e_interval_union_ns >= 0
        && static_cast<std::uint64_t>(out.benchmark_e2e_interval_union_ns)
            <= out.measured_region_ns
        && out.benchmark_e2e_interval_count > 0
        && (out.benchmark_e2e_cigar_interval_count > 0)
            == expect_cigar_interval
        && (out.benchmark_e2e_mapq_interval_count > 0)
            == expect_mapq_interval
        && campaign_origin_rows_coherent
        && out.benchmark_e2e_interval_origins.size() ==
            expected_origin_summary_count
        && interval_origins_cover_every_query("cigar", expect_cigar_interval)
        && interval_origins_cover_every_query("mapq", expect_mapq_interval);
    const bool selected_timing_gate =
        downstream_timing_gate && (!matrix_tuple || benchmark_interval_timing_gate);
    const bool accepted =
        stats_envelope_gate
        && attested_cardinality_gate
        && backend_identity_gate
        && algorithm_identity_gate
        && matrix_component_work_gate
        && matrix_legacy_gpu_index_build_gate
        && mapq_execution_gate
        && selected_timing_gate
        && row_equation
        && source_valid
        && persisted_identity_valid
        && persisted_read_gate
        && minimizer_fragment_reader_stats_gate
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
    out.pipeline_stats_consistent = accepted ? 1 : 0;
    if (accepted) {
        return true;
    }
    std::fprintf(
        stderr,
        "[bench_e2e] PIPELINE-VALIDATION-FAIL algo=%.*s present=%lld attached=%lld "
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
        static_cast<long long>(out.pipeline_stats_available),
        static_cast<long long>(out.pipeline_stats_attached_to_measurement),
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
        "[bench_e2e] PIPELINE-VALIDATION-FAIL-GATES "
        "validation_envelope=%d attested_cardinality=%d backend_identity=%d "
        "algorithm_identity=%d matrix_legacy_gpu_index_build=%d "
        "mapq_execution=%d "
        "row_equation=%d source_valid=%d persisted_identity=%d "
        "persisted_read=%d minimizer_fragment_reader=%d cpu_prepared=%d "
        "gpu_residency=%d "
        "simd_operational=%d scalar_operational=%d gpu_operational=%d "
        "persisted_gpu_operation=%d "
        "gpu_cache_identity=%d gpu_cache_runtime=%d "
        "cold_restart=%d cold_no_index=%d persisted_policy=%d "
        "backend_specific=%d manifest_size=%d cold_payload_equations=%d "
        "one_cold_route=%d selective_read=%d full_read=%d "
        "prepared_key_checksum=%s prepared_reader_checksum=%s\n",
        static_cast<int>(stats_envelope_gate),
        static_cast<int>(attested_cardinality_gate),
        static_cast<int>(backend_identity_gate),
        static_cast<int>(algorithm_identity_gate),
        static_cast<int>(matrix_legacy_gpu_index_build_gate),
        static_cast<int>(mapq_execution_gate),
        static_cast<int>(row_equation),
        static_cast<int>(source_valid),
        static_cast<int>(persisted_identity_valid),
        static_cast<int>(persisted_read_gate),
        static_cast<int>(minimizer_fragment_reader_stats_gate),
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
            "[bench_e2e] PIPELINE-VALIDATION-FAIL-GPU-CACHE "
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
            "[bench_e2e] PIPELINE-VALIDATION-FAIL-GPU-OPERATION "
            "gate=%d schema=%lld accepted=%lld complete=%lld "
            "saturated=%lld dropped=%lld operations=%lld/%lld "
            "jobs=%lld/%lld expected_jobs=%lld\n",
            static_cast<int>(persisted_gpu_operation_gate),
            static_cast<long long>(
                out.persisted_gpu_operation_stats_schema_valid),
            static_cast<long long>(
                out.persisted_gpu_operation_stats_accepted),
            static_cast<long long>(
                out.persisted_gpu_operation_stats_complete),
            static_cast<long long>(
                out.persisted_gpu_operation_stats_saturated),
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
            "[bench_e2e] PIPELINE-VALIDATION-FAIL-PERSISTED-READ "
            "route=%d session=%d head=%d manifest=%d directory=%d "
            "payload=%d scan_equations=%d zero_work=%d "
            "cold_schema=%lld pre_restart_gpu_abi=%lld "
            "grouped_soa_contract=%lld target_rows=%lld "
            "memory_passes=%lld memory_preparations=%lld "
            "memory_allocations=%lld append_growth=%lld retained=%lld/%lld "
            "position_bytes=%lld layout=%lld/%lld admitted=%lld "
            "exact_checks=%lld/%lld layout_checks=%lld/%lld "
            "memory_saturated=%lld\n",
            static_cast<int>(persisted_read_route_stats),
            static_cast<int>(persisted_read_session_stats),
            static_cast<int>(persisted_read_head_stats),
            static_cast<int>(persisted_read_manifest_stats),
            static_cast<int>(persisted_read_directory_stats),
            static_cast<int>(persisted_read_payload_stats),
            static_cast<int>(persisted_read_scan_equations),
            static_cast<int>(persisted_read_zero_work_stats),
            static_cast<long long>(out.cold_index_schema_valid),
            static_cast<long long>(out.pre_restart_persisted_gpu_abi_version),
            static_cast<long long>(
                out.persisted_format5_manifest_contract_valid),
            static_cast<long long>(out.target_rows),
            static_cast<long long>(
                out.grouped_soa_reader_capacity_accounting_passes),
            static_cast<long long>(
                out.grouped_soa_reader_global_position_storage_preparations),
            static_cast<long long>(
                out.grouped_soa_reader_global_position_storage_allocations),
            static_cast<long long>(
                out.grouped_soa_reader_position_append_growth_reservations),
            static_cast<long long>(
                out.grouped_soa_reader_retained_position_count),
            static_cast<long long>(
                out.grouped_soa_reader_retained_position_capacity),
            static_cast<long long>(
                out.grouped_soa_reader_global_position_capacity_bytes),
            static_cast<long long>(
                out.grouped_soa_reader_owned_capacity_bytes),
            static_cast<long long>(grouped_soa_reader_layout_bytes),
            static_cast<long long>(out.grouped_soa_reader_admitted_bytes),
            static_cast<long long>(
                out.grouped_soa_reader_exact_position_image_checks),
            static_cast<long long>(
                out.grouped_soa_reader_exact_position_image_failures),
            static_cast<long long>(
                out.grouped_soa_reader_retained_layout_checks),
            static_cast<long long>(
                out.grouped_soa_reader_retained_layout_failures),
            static_cast<long long>(
                out.are_grouped_soa_reader_memory_stats_saturated));
        std::fputs(
            "[bench_e2e] PIPELINE-VALIDATION-FAIL-PERSISTED-COUNTERS",
            stderr);
        for (std::size_t column = 48; column <= 85; ++column) {
            const std::string_view name =
                k_alignment_snapshot_column_names[column];
            std::fprintf(
                stderr, " %.*s=%lld",
                static_cast<int>(name.size()), name.data(),
                static_cast<long long>(counter(column)));
        }
        std::fputc('\n', stderr);
    }
    std::fprintf(
        stderr,
        "[bench_e2e] PIPELINE-VALIDATION-FAIL-SIMD "
        "requested_policy=%s observed_policy=%s "
        "requested_alignment=%s observed_alignment=%s "
        "json_matches_prefix=%d no_simd_stats=%d bounded_simd_stats=%d "
        "no_block_stats=%d accepted_block_stats=%d "
        "block_present=%lld block_accepted=%lld block_calls=%lld "
        "block_nonempty=%lld block_nonempty_simd=%lld "
        "block_vector_batches=%lld block_cells=%lld block_ksw2=%lld "
        "block_scalar_fallbacks=%lld block_saturated=%lld\n",
        out.requested_simd_chaining_policy.c_str(),
        out.observed_simd_chaining_policy.c_str(),
        out.requested_simd_alignment_algorithm.c_str(),
        out.observed_simd_alignment_algorithm.c_str(),
        static_cast<int>(simd_json_counters_match_prefix),
        static_cast<int>(no_simd_policy_stats),
        static_cast<int>(bounded_simd_policy_stats),
        static_cast<int>(no_block_aligner_execution_stats),
        static_cast<int>(accepted_block_aligner_execution_stats),
        static_cast<long long>(out.block_aligner_execution_stats_available),
        static_cast<long long>(out.block_aligner_execution_stats_consistent),
        static_cast<long long>(out.block_aligner_executed_calls),
        static_cast<long long>(out.block_aligner_nonempty_calls),
        static_cast<long long>(out.block_aligner_nonempty_calls_with_simd),
        static_cast<long long>(out.block_aligner_simd_dp_vector_batches),
        static_cast<long long>(out.block_aligner_simd_dp_cells),
        static_cast<long long>(out.block_aligner_forbidden_ksw2_calls),
        static_cast<long long>(
            out.block_aligner_forbidden_scalar_biological_fallbacks),
        static_cast<long long>(out.block_aligner_execution_stats_saturated));
    std::fflush(stderr);
    if (matrix_tuple) {
        out.tuple_accepted = 0;
        out.cell_disposition = "rejected_attestation";
        out.phase = "attestation";
        out.tuple_rejection_reason = fmt::format(
            "pipeline attestation rejected tuple requested_backend={} "
            "actual_backend={} stage_engine_matrix_valid={} "
            "declines={} fallbacks={} prefilter_activity={}",
            out.requested_backend,
            out.actual_backend,
            out.stage_engine_matrix_valid,
            out.backend_declines,
            out.backend_fallbacks,
            out.prefilter_activity_count);
        return;
    }
    std::abort();
}

// create_e2e_table: a USING bioseq table matching the active alphabet and the
// E2E generator's uint64 storage width.
void disable_e2e_autovacuum(const it::connection_test_context& c,
                            const std::string& table) {
    it::exec_sqlf(c, "ALTER TABLE {} SET (autovacuum_enabled = false);", table);
}

[[nodiscard]] bool is_exact_command_ok(
        const captured_query_rows& captured) noexcept {
    return captured.status == captured_query_status::command_ok
        && captured.result_status == "PGRES_COMMAND_OK"
        && captured.rows.empty()
        && captured.connection_diagnostic.empty()
        && captured.sqlstate.empty()
        && captured.primary.empty()
        && captured.detail.empty()
        && captured.hint.empty();
}

[[noreturn]] void abort_unexpected_e2e_table_setup_result(
        const captured_query_rows& captured,
        const std::string& table) {
    std::fprintf(
        stderr,
        "[bench_e2e] TABLE-SETUP-FATAL table=%s status=%d "
        "result_status=%s sqlstate=%s primary=%s detail=%s hint=%s "
        "connection=%s\n",
        table.c_str(),
        static_cast<int>(captured.status),
        captured.result_status.c_str(),
        captured.sqlstate.c_str(),
        captured.primary.c_str(),
        captured.detail.c_str(),
        captured.hint.c_str(),
        captured.connection_diagnostic.c_str());
    std::fflush(stderr);
    std::abort();
}

[[nodiscard]] captured_query_rows create_e2e_table_capturing_error(
        it::connection_test_context& c,
        const std::string& table,
        const std::string& seq_type) {
    it::exec_sqlf(c, "DROP TABLE IF EXISTS {} CASCADE;", table);
    captured_query_rows captured = query_rows_capturing_error(
        c,
        fmt::format(
            "CREATE TABLE {} (header text, seq {}) USING bioseq;",
            table,
            seq_type));
    if (is_exact_command_ok(captured)) {
        disable_e2e_autovacuum(c, table);
    }
    return captured;
}

void create_e2e_table(it::connection_test_context& c, const std::string& t,
                      const std::string& seq_type) {
    const captured_query_rows captured =
        create_e2e_table_capturing_error(c, t, seq_type);
    if (!is_exact_command_ok(captured)) {
        abort_unexpected_e2e_table_setup_result(captured, t);
    }
}

void configure_bioseq_aligner_session(it::connection_test_context& c,
                                      int kmer_size,
                                      int window_size) {
    it::exec_sql(c, "SET bioseq.aligner.base_alignment.algorithm = 'block_aligner';");
    it::exec_sql(c, "SET bioseq.aligner.chaining.algorithm       = 'simd_minimap2';");
    it::exec_sql(c, "SET bioseq.aligner.minimizer.algorithm      = 'two_stack_nthash';");
    it::exec_sqlf(c, "SET bioseq.aligner.minimizer.kmer_size   = {};", kmer_size);
    it::exec_sqlf(c, "SET bioseq.aligner.minimizer.window_size = {};", window_size);
}

void configure_bioseq_simd_session(
    it::connection_test_context& connection,
    int kmer_size,
    int window_size)
{
    configure_bioseq_aligner_session(connection, kmer_size, window_size);
    it::exec_sql(connection, "SET bioseq.aligner.vulkan.enabled = off;");
    it::exec_sql(connection, "SET bioseq.aligner.vulkan.seed_lookup.enabled = off;");
    it::exec_sql(connection, "SET bioseq.aligner.vulkan.index_cache.enabled = off;");
    it::exec_sql(connection, "SET bioseq.aligner.vulkan.fused_frontend.enabled = off;");
    it::exec_sql(connection, "SET bioseq.aligner.vulkan.index_build.enabled = off;");
    it::exec_sql(connection, "SET bioseq.aligner.vulkan.alignment_extension.enabled = off;");
}

void configure_bioseq_scalar_session(
    it::connection_test_context& connection,
    int kmer_size,
    int window_size)
{
    configure_bioseq_aligner_session(connection, kmer_size, window_size);
    it::exec_sql(
        connection,
        "SET bioseq.aligner.base_alignment.algorithm = 'ksw2';");
    it::exec_sql(
        connection,
        "SET bioseq.aligner.chaining.algorithm = 'scalar_minimap2';");
    it::exec_sql(
        connection,
        "SET bioseq.aligner.minimizer.algorithm = 'scalar';");
    it::exec_sql(connection, "SET bioseq.aligner.vulkan.enabled = off;");
    it::exec_sql(connection, "SET bioseq.aligner.vulkan.seed_lookup.enabled = off;");
    it::exec_sql(connection, "SET bioseq.aligner.vulkan.index_cache.enabled = off;");
    it::exec_sql(connection, "SET bioseq.aligner.vulkan.fused_frontend.enabled = off;");
    it::exec_sql(connection, "SET bioseq.aligner.vulkan.index_build.enabled = off;");
    it::exec_sql(connection, "SET bioseq.aligner.vulkan.alignment_extension.enabled = off;");
}

// Full Vulkan minimizer and GPU-index configuration. Apply it before persisted
// index construction so the untimed just-index setup uses the measured backend.
void configure_bioseq_vulkan_index_session(
    it::connection_test_context& connection,
    int kmer_size,
    int window_size,
    bool enable_extension = false)
{
    configure_bioseq_aligner_session(connection, kmer_size, window_size);
    it::exec_sql(
        connection,
        "SET bioseq.aligner.chaining.algorithm = 'vulkan_tiled_dynamic_programming';");
    it::exec_sql(
        connection,
        "SET bioseq.aligner.minimizer.algorithm = 'vulkan_minimap_integer';");
    it::exec_sql(
        connection,
        "SET bioseq.aligner.vulkan.enabled = on;");
    it::exec_sql(
        connection,
        "SET bioseq.aligner.vulkan.seed_lookup.enabled = on;");
    it::exec_sql(
        connection,
        "SET bioseq.aligner.vulkan.index_cache.enabled = on;");
    it::exec_sql(
        connection,
        "SET bioseq.aligner.vulkan.fused_frontend.enabled = on;");
    it::exec_sql(
        connection,
        "SET bioseq.aligner.vulkan.index_build.enabled = on;");
    it::exec_sqlf(
        connection,
        "SET bioseq.aligner.vulkan.alignment_extension.enabled = {};",
        enable_extension ? "on" : "off");
}

[[nodiscard]] std::string param_string(const parameter_map& params,
                                       std::string_view name,
                                       std::string_view fallback) {
    const auto it = params.find(std::string{name});
    if (it == params.end() || !it->second.is_string()) {
        return std::string{fallback};
    }
    return std::string{it->second.as_string()};
}

[[nodiscard]] int param_int(const parameter_map& params,
                            std::string_view name,
                            int fallback) {
    const auto it = params.find(std::string{name});
    if (it == params.end() || !it->second.is_integer()) {
        return fallback;
    }
    return static_cast<int>(it->second.as_integer());
}

[[nodiscard]] std::string algorithm_matrix_identity(
        std::string_view minimizer,
        std::string_view minimizer_index) {
    return fmt::format("{}|{}", minimizer, minimizer_index);
}

/** Return the exact persisted producer selected by one canonical minimizer. */
[[nodiscard]] std::optional<std::string_view>
algorithm_matrix_persisted_backend(std::string_view minimizer) noexcept {
    if (minimizer == "scalar" || minimizer == "simd_minimap2_reference") {
        return std::string_view{"scalar"};
    }
    if (minimizer == "two_stack_nthash") {
        return std::string_view{"exact_simd"};
    }
    if (minimizer == "vulkan_minimap_integer") {
        return std::string_view{"vulkan_segment"};
    }
    if (minimizer == "bounded_nthash_simd") {
        return std::string_view{"bounded_nthash_simd"};
    }
    return std::nullopt;
}

/** Return whether every cell sharing this persisted setup must reject untimed. */
[[nodiscard]] bool algorithm_matrix_persisted_setup_must_reject(const e2e_input& in,
                                                                std::string_view minimizer,
                                                                std::string_view minimizer_index) noexcept {
    const bool canonical_index =
        minimizer_index == k_minimizers_per_fragment_index ||
        minimizer_index == k_minimizers_per_sequence_index;
    return !canonical_index ||
        e2e_minimizer_runtime_profile(in, minimizer) == nullptr;
}

struct algorithm_matrix_vulkan_hardware_detail {
    std::int64_t bp = 0;
    std::int64_t padded_bytes = 0;
    std::int64_t max_min = 0;
    std::int64_t query_gate_words = 0;
    std::int64_t window_size = 0;
    std::int64_t kmer_size = 0;
};

/** Strictly parse the production Vulkan hardware-decline diagnostic. */
[[nodiscard]] std::optional<algorithm_matrix_vulkan_hardware_detail>
parse_algorithm_matrix_vulkan_hardware_detail(
        std::string_view detail) noexcept {
    constexpr std::string_view prefix =
        "Vulkan segment minimizer pair path selected but strict Vulkan "
        "execution was rejected (reason=hardware_device_unavailable bp=";
    if (!detail.starts_with(prefix)) {
        return std::nullopt;
    }
    detail.remove_prefix(prefix.size());

    const auto parse_number = [](std::string_view token)
            -> std::optional<std::int64_t> {
        if (token.empty() || (token.size() > 1u && token.front() == '0')) {
            return std::nullopt;
        }
        for (const char ch : token) {
            if (ch < '0' || ch > '9') {
                return std::nullopt;
            }
        }
        return parse_i64_exact(token);
    };
    const auto take_number = [&detail, &parse_number](std::string_view delimiter)
            -> std::optional<std::int64_t> {
        const std::size_t position = detail.find(delimiter);
        if (position == std::string_view::npos) {
            return std::nullopt;
        }
        const auto value = parse_number(detail.substr(0, position));
        detail.remove_prefix(position + delimiter.size());
        return value;
    };
    const auto bp = take_number(" padded_bytes=");
    const auto padded_bytes = take_number(" max_min=");
    const auto max_min = take_number(" query_gate_words=");
    const auto query_gate_words = take_number(" w=");
    const auto window_size = take_number(" k=");
    if (detail.empty() || detail.back() != ')') {
        return std::nullopt;
    }
    const auto kmer_size = parse_number(
        detail.substr(0, detail.size() - 1u));
    if (!bp.has_value() || !padded_bytes.has_value()
        || !max_min.has_value() || !query_gate_words.has_value()
        || !window_size.has_value() || !kmer_size.has_value()
        || *bp <= 0 || *padded_bytes < 0 || *max_min < 0
        || *query_gate_words < 0 || *window_size < 0 || *kmer_size < 0) {
        return std::nullopt;
    }
    return algorithm_matrix_vulkan_hardware_detail{
        *bp,
        *padded_bytes,
        *max_min,
        *query_gate_words,
        *window_size,
        *kmer_size,
    };
}

/** Match only singleton0704's persisted Vulkan hardware capability boundary. */
[[nodiscard]] bool is_allowlisted_algorithm_matrix_vulkan_hardware_rejection(
        const captured_query_rows& captured,
        const e2e_input& in,
        std::string_view minimizer,
        std::string_view minimizer_index) noexcept {
    const bool canonical_index =
        minimizer_index == k_minimizers_per_fragment_index
        || minimizer_index == k_minimizers_per_sequence_index;
    const auto diagnostic =
        parse_algorithm_matrix_vulkan_hardware_detail(captured.detail);
    // This predicate is called only by the just_index setup/serializer paths.
    return in.size_label == "matrix_2gbp_dna4_k15_w10"
        && in.alphabet == "dna4"
        && in.kmer_size == 15
        && in.window_size == 10
        && in.cigar_projection
        && in.mapq_projection
        && !in.prefilter_enabled
        && minimizer == "vulkan_minimap_integer"
        && canonical_index
        && captured.status == captured_query_status::server_error
        && captured.result_status == "PGRES_FATAL_ERROR"
        && captured.sqlstate == "XX000"
        && captured.primary == "bioseq.update_index raised a C++ exception"
        && captured.hint.empty()
        && diagnostic.has_value()
        && diagnostic->window_size == in.window_size
        && diagnostic->kmer_size == in.kmer_size;
}

/** Match only the campaign's exact persisted setup capability diagnostics. */
[[nodiscard]] bool is_allowlisted_algorithm_matrix_setup_rejection(
        const captured_query_rows& captured,
        const e2e_input& in,
        std::string_view minimizer,
        std::string_view minimizer_index) {
    const auto backend = algorithm_matrix_persisted_backend(minimizer);
    if (!backend.has_value()
        || captured.status != captured_query_status::server_error
        || captured.result_status != "PGRES_FATAL_ERROR"
        || captured.sqlstate != "0A000") {
        return false;
    }

    if (in.kmer_size != 15 || in.window_size != 10
        || in.alphabet != "dna5") {
        return false;
    }
    if (minimizer == "bounded_nthash_simd") {
        return captured.primary ==
                "Bounded ntHash SIMD minimizer index build is unsupported for this source"
            && captured.detail ==
                "bioseq.update_index: status=unsupported_alphabet "
                "grouped minimizer generation preflight: checked bounded ntHash route "
                "failed (unsupported_alphabet)";
    }
    return captured.primary ==
            "READY minimizer backend has no canonical grouped-SoA profile"
        && captured.detail == fmt::format(
            "backend={} wire_error=24", *backend);
}

/** Match only the production dna5 typmod capability boundary. */
[[nodiscard]] bool
is_allowlisted_algorithm_matrix_table_setup_rejection(
        const captured_query_rows& captured,
        const e2e_input& in) noexcept {
    return in.alphabet == "dna5"
        && captured.status == captured_query_status::server_error
        && captured.result_status == "PGRES_FATAL_ERROR"
        && captured.sqlstate == "22023"
        && captured.primary ==
            "bioseq.seq alphabet \"dna5\" is not supported"
        && captured.detail.empty()
        && captured.hint == "Valid alphabets: 'dna4'";
}

[[nodiscard]] std::string algorithm_matrix_table_name(
        const e2e_input& in,
        const e2e_just_index_setup_requirements::matrix_setup& setup) {
    std::uint64_t digest = 1469598103934665603ull;
    const std::string full_identity = fmt::format(
        "{}|{}|k{}|w{}|{}", in.size_label, in.alphabet, in.kmer_size,
        in.window_size, setup.identity);
    for (const unsigned char byte : full_identity) {
        digest ^= byte;
        digest *= 1099511628211ull;
    }
    return fmt::format("e2em_{}_k{}_w{}_{:016x}",
        in.alphabet, in.kmer_size, in.window_size, digest);
}

[[nodiscard]] std::string algorithm_matrix_query_table_name(
        const e2e_input& in,
        const std::string_view test_case_name,
        const std::size_t query_count,
        const std::uint64_t seed) {
    std::uint64_t digest = 1469598103934665603ull;
    const std::string full_identity = fmt::format(
        "{}|{}|{}|k{}|w{}|q{}|seed{}", test_case_name, in.size_label,
        in.alphabet, in.kmer_size, in.window_size, query_count, seed);
    for (const unsigned char byte : full_identity) {
        digest ^= byte;
        digest *= 1099511628211ull;
    }
    return fmt::format("e2eq_{}_k{}_w{}_{:016x}",
        in.alphabet, in.kmer_size, in.window_size, digest);
}

/** Require exact READY identity; an absent generation requests fresh setup. */
[[nodiscard]] bool validate_reusable_algorithm_matrix_target(
        const e2e_input& in,
        const e2e_just_index_setup_requirements::matrix_setup& setup,
        const std::string& table,
        const std::int64_t sequence_rowno,
        const std::string_view generated_target_content_crc64) {
    const auto& profile = require_e2e_minimizer_runtime_profile(
        in, setup.minimizer_algorithm);
    // Persisted MPF records use fragment-local positions, whereas execution
    // profiles cover the full target. Use the builder/inspector's selector so
    // a valid fragment record is not compared with a whole-target record.
    const auto index_identity =
        ::bioseq::indexing::select_minimizer_generation_algorithm_identity_runtime(
            profile.requested_algorithm,
            setup.minimizer_index == k_minimizers_per_fragment_index
                ? ::bioseq::indexing::k_minimizer_fragment_algorithm_position_extent_bp
                : static_cast<std::uint64_t>(in.target_bp),
            profile.alphabet_id,
            profile.packed_storage_bits,
            static_cast<std::uint16_t>(in.kmer_size),
            static_cast<std::uint16_t>(in.window_size));
    const auto producer_backend =
        index_identity
            ? ::bioseq::indexing::minimizer_sketch_backend_for_effective_algorithm(
                  index_identity->effective_algorithm)
            : std::nullopt;
    const auto producer = producer_backend.has_value()
        ? ::bioseq::indexing::minimizer_producer_backend_identity_for_backend(
              *producer_backend)
        : ::bioseq::indexing::minimizer_producer_identity{};
    if (!index_identity
        || !::bioseq::indexing::is_supported_minimizer_producer_backend_identity(
            producer)) {
        std::fputs(
            "[bench_e2e] MATRIX-TARGET-REUSE-FATAL generated producer identity is invalid\n",
            stderr);
        std::fflush(stderr);
        std::abort();
    }

    const bool gpu_seed_selected =
        setup.minimizer_algorithm == "vulkan_minimap_integer";
    if (e2e_expects_hash_major(
            in, setup.minimizer_index, gpu_seed_selected)) {
        const nlohmann::json audit = capture_hash_major_ready_stats(
            *in.conn, table, setup.minimizer_index, sequence_rowno, in,
            setup.minimizer_algorithm);
        const nlohmann::json& source = required_json_object(audit, "source");
        const nlohmann::json& manifest = required_json_object(audit, "manifest");
        const nlohmann::json& compatibility =
            required_json_object(manifest, "compatibility");
        if (required_json_text(source, "source_content_crc64")
                != generated_target_content_crc64
            || required_json_counter(
                    compatibility, "producer_backend_identity")
                != static_cast<std::int64_t>(producer.id)
            || required_json_counter(
                    compatibility, "producer_backend_revision")
                != static_cast<std::int64_t>(producer.revision)) {
            std::fprintf(
                stderr,
                "[bench_e2e] MATRIX-TARGET-REUSE-FATAL retained hash-major "
                "target content/producer mismatch table=%s index=%s\n",
                table.c_str(), setup.minimizer_index.c_str());
            std::fflush(stderr);
            std::abort();
        }
        return true;
    }

    const auto captured = query_rows_capturing_error(
        *in.conn,
        fmt::format(
            "SELECT generation_id, source_identity, compatibility_checksum, "
            "source_content_checksum, sequence_rowno, num_fragments, "
            "total_bp_size, alphabet_code, storage_unit_byte_width, kmer_size, "
            "window_size, requested_algorithm_id, effective_algorithm_id, "
            "physical_record_id, highway_owner_id, record_position_log2, "
            "physical_kmer_size, hash_width_bytes, packed_storage_bits, "
            "payload_layout_abi_revision, producer_backend_id, "
            "producer_identity_revision, gpu_abi_revision "
            "FROM bioseq.inspect_ready_minimizer_generation("
            "'{}'::regclass, '{}', {})",
            table, setup.minimizer_index, sequence_rowno));
    if (captured.status == captured_query_status::server_error
        && captured.sqlstate == "55000"
        && captured.primary == "bioseq current-generation index: generation relation is missing or renamed") {
        // An interrupted index build may leave the source row but no generation.
        // This is not a reusable index; the caller performs normal untimed setup.
        // Do not reinterpret corrupt/mismatched READY data or other SQL failures.
        return false;
    }
    if (captured.status != captured_query_status::tuples_ok) {
        std::fprintf(stderr,
            "[bench_e2e] MATRIX-TARGET-REUSE-FATAL index inspection failed "
            "table=%s sqlstate=%s primary=%s connection=%s\n",
            table.c_str(), captured.sqlstate.c_str(), captured.primary.c_str(),
            captured.connection_diagnostic.c_str());
        std::fflush(stderr);
        std::abort();
    }
    const auto& rows = captured.rows;
    const bool shape_valid = rows.size() == 1u && rows.front().size() == 23u
        && std::ranges::all_of(
            rows.front(), [](const auto& value) noexcept {
                return value.has_value();
            });
    const auto integer_at = [&rows, shape_valid](const std::size_t column)
            -> std::optional<std::int64_t> {
        return shape_valid ? parse_i64_exact(*rows.front()[column]) : std::nullopt;
    };
    const auto generation_id = integer_at(0);
    const auto observed_sequence_rowno = integer_at(4);
    const auto num_fragments = integer_at(5);
    const auto total_bp_size = integer_at(6);
    const auto alphabet_code = integer_at(7);
    const auto storage_unit_byte_width = integer_at(8);
    const auto kmer_size = integer_at(9);
    const auto window_size = integer_at(10);
    const auto requested_algorithm_id = integer_at(11);
    const auto effective_algorithm_id = integer_at(12);
    const auto physical_record_id = integer_at(13);
    const auto highway_owner_id = integer_at(14);
    const auto record_position_log2 = integer_at(15);
    const auto physical_kmer_size = integer_at(16);
    const auto hash_width_bytes = integer_at(17);
    const auto packed_storage_bits = integer_at(18);
    const auto payload_layout_abi_revision = integer_at(19);
    const auto producer_backend_id = integer_at(20);
    const auto producer_identity_revision = integer_at(21);
    const auto gpu_abi_revision = integer_at(22);
    const bool exact = shape_valid
        && fixed_lower_hex(*rows.front()[1], 16)
        && fixed_lower_hex(*rows.front()[2], 16)
        && fixed_lower_hex(*rows.front()[3], 16)
        && *rows.front()[3] == generated_target_content_crc64
        && generation_id.has_value() && *generation_id > 0
        && observed_sequence_rowno == sequence_rowno
        && num_fragments.has_value() && *num_fragments > 0
        && total_bp_size == in.target_bp
        && alphabet_code == static_cast<std::int64_t>(index_identity->alphabet_id)
        && storage_unit_byte_width
            == static_cast<std::int64_t>(sizeof(bench_storage_t))
        && kmer_size == in.kmer_size
        && window_size == in.window_size
        && requested_algorithm_id == static_cast<std::int64_t>(
            index_identity->requested_algorithm)
        && effective_algorithm_id == static_cast<std::int64_t>(
            index_identity->effective_algorithm)
        && physical_record_id == static_cast<std::int64_t>(
            index_identity->physical_record_id)
        && highway_owner_id == static_cast<std::int64_t>(
            index_identity->highway_owner_id)
        && record_position_log2 == static_cast<std::int64_t>(
            index_identity->record_position_log2)
        && physical_kmer_size == static_cast<std::int64_t>(
            index_identity->physical_kmer_size)
        && hash_width_bytes == static_cast<std::int64_t>(index_identity->hash_width)
        && packed_storage_bits == static_cast<std::int64_t>(
            index_identity->packed_storage_bits)
        && payload_layout_abi_revision == static_cast<std::int64_t>(
            index_identity->payload_layout_abi_revision)
        && producer_backend_id == static_cast<std::int64_t>(producer.id)
        && producer_identity_revision == static_cast<std::int64_t>(
            producer.revision)
        && gpu_abi_revision == static_cast<std::int64_t>(
            ::bioseq::indexing::k_grouped_soa_gpu_abi_revision);
    if (!exact) {
        std::fprintf(
            stderr,
            "[bench_e2e] MATRIX-TARGET-REUSE-FATAL retained READY target "
            "does not match generated content/runtime identity table=%s "
            "index=%s generated_crc64=%.*s persisted_crc64=%s\n",
            table.c_str(), setup.minimizer_index.c_str(),
            static_cast<int>(generated_target_content_crc64.size()),
            generated_target_content_crc64.data(),
            shape_valid ? rows.front()[3]->c_str() : "<invalid>");
        std::fflush(stderr);
        std::abort();
    }
    return true;
}

[[nodiscard]] std::array<std::string, 22>
render_bioseq_algorithm_matrix_session_sql(
        int kmer_size,
        int window_size,
        std::string_view minimizer,
        std::string_view chaining,
        std::string_view alignment,
        std::string_view pipeline_mode,
        std::string_view minimizer_index,
        bool build_target) {
    const bool vulkan_seed = minimizer == "vulkan_minimap_integer";
    const auto setting = [](const bool enabled) noexcept -> std::string_view {
        return enabled ? "on" : "off";
    };
    return {{
        "SET bioseq.planner.custom_scan.enabled = on;",
        "SET bioseq.planner.index_scan.enabled = on;",
        "SET bioseq.vectorization.enabled = on;",
        fmt::format(
            "SET bioseq.scheduler.pipeline.mode = '{}';", pipeline_mode),
        fmt::format(
            "SET bioseq.aligner.base_alignment.algorithm = '{}';", alignment),
        fmt::format(
            "SET bioseq.aligner.chaining.algorithm = '{}';", chaining),
        fmt::format(
            "SET bioseq.aligner.minimizer.algorithm = '{}';", minimizer),
        fmt::format(
            "SET bioseq.aligner.minimizer.kmer_size = {};", kmer_size),
        fmt::format(
            "SET bioseq.aligner.minimizer.window_size = {};", window_size),
        "SET bioseq.aligner.grouped_soa_generation.enabled = on;",
        "SET bioseq.aligner.vulkan.enabled = on;",
        fmt::format(
            "SET bioseq.aligner.vulkan.seed_lookup.enabled = {};",
            setting(vulkan_seed)),
        "SET bioseq.aligner.vulkan.index_cache.enabled = on;",
        "SET bioseq.aligner.vulkan.fused_frontend.enabled = off;",
        "SET bioseq.aligner.vulkan.alignment_extension.enabled = off;",
        fmt::format(
            "SET bioseq.aligner.vulkan.index_build.enabled = {};",
            setting(vulkan_seed && build_target)),
        fmt::format(
            "SET bioseq.aligner.fragment_minimizer.lookup_route = '{}';",
            minimizer_index == k_minimizers_per_fragment_index
                ? "direct_compact_hash_lookup" : "control_block_scan"),
        "SET bioseq.column_cache.enabled = on;",
        "SET bioseq.index.async_persistence.enabled = off;",
        "SET bioseq.index.recompute_async_persistence.enabled = off;",
        "SET bioseq.index.insert_maintenance.enabled = off;",
        fmt::format(
            "SET bioseq.index.sequence_read.enabled = {};",
            setting(!build_target)),
    }};
}

// The paired/chunk diagnostic predates the Gate-8 single-statement boundary.
// Keep its exact 12-setting route recipe stable; only non-paired matrix rows
// use the complete campaign session below.
void configure_bioseq_algorithm_matrix_legacy_session(
        it::connection_test_context& connection,
        int kmer_size,
        int window_size,
        std::string_view minimizer,
        std::string_view chaining,
        std::string_view alignment,
        bool build_target) {
    const bool vulkan_seed = minimizer == "vulkan_minimap_integer";
    const bool vulkan_chain = chaining == "vulkan_tiled_dynamic_programming";
    const bool vulkan = vulkan_seed || vulkan_chain;
    const auto setting = [](const bool enabled) noexcept -> std::string_view {
        return enabled ? "on" : "off";
    };
    const std::array<std::string, 12> sql{{
        fmt::format(
            "SET bioseq.aligner.base_alignment.algorithm = '{}';", alignment),
        fmt::format(
            "SET bioseq.aligner.chaining.algorithm = '{}';", chaining),
        fmt::format(
            "SET bioseq.aligner.minimizer.algorithm = '{}';", minimizer),
        fmt::format(
            "SET bioseq.aligner.minimizer.kmer_size = {};", kmer_size),
        fmt::format(
            "SET bioseq.aligner.minimizer.window_size = {};", window_size),
        "SET bioseq.aligner.grouped_soa_generation.enabled = on;",
        fmt::format(
            "SET bioseq.aligner.vulkan.enabled = {};", setting(vulkan)),
        fmt::format(
            "SET bioseq.aligner.vulkan.seed_lookup.enabled = {};",
            setting(vulkan_seed)),
        fmt::format(
            "SET bioseq.aligner.vulkan.index_cache.enabled = {};",
            setting(vulkan_seed)),
        "SET bioseq.aligner.vulkan.fused_frontend.enabled = off;",
        fmt::format(
            "SET bioseq.aligner.vulkan.index_build.enabled = {};",
            setting(vulkan_seed && build_target)),
        "SET bioseq.aligner.vulkan.alignment_extension.enabled = off;",
    }};
    for (const std::string& statement : sql) {
        it::exec_sql(connection, statement);
    }
}

void configure_bioseq_algorithm_matrix_legacy_session(
        it::connection_test_context& connection,
        const e2e_input& in,
        const parameter_map& params,
        bool build_target) {
    configure_bioseq_algorithm_matrix_legacy_session(
        connection, in.kmer_size, in.window_size,
        param_string(params, "minimizer_algorithm", "scalar"),
        param_string(params, "chaining_algorithm", "scalar_minimap2"),
        param_string(params, "base_alignment_algorithm", "ksw2"),
        build_target);
}

void configure_bioseq_algorithm_matrix_session(
        it::connection_test_context& connection,
        int kmer_size,
        int window_size,
        std::string_view minimizer,
        std::string_view chaining,
        std::string_view alignment,
        std::string_view pipeline_mode,
        std::string_view minimizer_index,
        bool build_target) {
    for (const std::string& sql : render_bioseq_algorithm_matrix_session_sql(
             kmer_size,
             window_size,
             minimizer,
             chaining,
             alignment,
             pipeline_mode,
             minimizer_index,
             build_target)) {
        it::exec_sql(connection, sql);
    }
}

void configure_bioseq_algorithm_matrix_session(
        it::connection_test_context& connection,
        const e2e_input& in,
        const parameter_map& params,
        std::string_view pipeline_mode,
        std::string_view minimizer_index,
        bool build_target) {
    configure_bioseq_algorithm_matrix_session(
        connection, in.kmer_size, in.window_size,
        param_string(params, "minimizer_algorithm", "scalar"),
        param_string(params, "chaining_algorithm", "scalar_minimap2"),
        param_string(params, "base_alignment_algorithm", "ksw2"),
        pipeline_mode,
        minimizer_index,
        build_target);
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

[[nodiscard]] std::string param_minimizer_index(const parameter_map& params) {
    std::string index =
        param_string(params, "minimizer_index", k_minimizers_per_fragment_index);
    if (index != k_minimizers_per_fragment_index
        && index != k_minimizers_per_sequence_index) {
        bioseq_log<logging_level::error>("[bench_e2e] unsupported minimizer_index '{}'", index);
        std::abort();
    }
    return index;
}

[[nodiscard]] std::string param_sequence_minimizer_sort_algorithm(
        const parameter_map& params) {
    std::string impl =
        param_string(params, "sequence_minimizer_sort_algorithm", "std_sort");
    if (impl != "std_sort"
        && impl != "simd_u128_vqsort"
        && impl != "simd_u128_vec_stream_1"
        && impl != "simd_u128_vec_stream_2"
        && impl != "simd_u128_vec_stream_4") {
        bioseq_log<logging_level::error>(
            "[bench_e2e] unsupported sequence_minimizer_sort_algorithm '{}'",
            impl);
        std::abort();
    }
    return impl;
}

void configure_bioseq_pair_sort_session(
        it::connection_test_context& c,
        const parameter_map& params) {
    const std::string impl = param_sequence_minimizer_sort_algorithm(params);
    it::exec_sqlf(
        c,
        "SET bioseq.index.sequence_minimizer_sort.algorithm = '{}';",
        impl);
}

void configure_bioseq_scheduler_session(it::connection_test_context& c,
                                        std::string_view pipeline_mode) {
    it::exec_sql(c, "SET max_parallel_workers_per_gather = 0;");
    it::exec_sqlf(c, "SET bioseq.scheduler.pipeline.mode = '{}';",
                  pipeline_mode);
    it::exec_sql(c, "SET bioseq.index.async_persistence.enabled = on;");
}

void configure_bioseq_minimizer_policy(
        it::connection_test_context& c,
        const std::string& table,
        std::string_view minimizer_index) {
    if (minimizer_index == k_minimizers_per_sequence_index) {
        it::exec_sqlf(c,
            "SELECT bioseq.set_index_policy('{}'::regclass, '{}', 'UPDATE_MANUALLY');",
            table, k_minimizers_per_fragment_index);
        it::exec_sqlf(c,
            "SELECT bioseq.set_index_policy('{}'::regclass, '{}', 'UPDATE_ON_INSERT');",
            table, k_minimizers_per_sequence_index);
        it::exec_sqlf(c,
            "SELECT bioseq.set_active_read_index('{}'::regclass, '{}');",
            table, k_minimizers_per_sequence_index);
    } else {
        it::exec_sqlf(c,
            "SELECT bioseq.set_index_policy('{}'::regclass, '{}', 'UPDATE_ON_INSERT');",
            table, k_minimizers_per_fragment_index);
        it::exec_sqlf(c,
            "SELECT bioseq.set_index_policy('{}'::regclass, '{}', 'UPDATE_MANUALLY');",
            table, k_minimizers_per_sequence_index);
        it::exec_sqlf(c,
            "SELECT bioseq.set_active_read_index('{}'::regclass, '{}');",
            table, k_minimizers_per_fragment_index);
    }
}

[[nodiscard]] const std::string& just_index_table_for(
        const e2e_input& in,
        std::string_view minimizer_index,
        just_index_setup_kind setup_kind = just_index_setup_kind::simd) {
    if (minimizer_index == k_minimizers_per_sequence_index) {
        switch (setup_kind) {
            case just_index_setup_kind::scalar:
                return in.just_index_table_scalar_per_sequence;
            case just_index_setup_kind::simd:
                return in.just_index_table_simd_per_sequence;
            case just_index_setup_kind::gpu:
                return in.just_index_table_gpu_per_sequence;
        }
    }
    switch (setup_kind) {
        case just_index_setup_kind::scalar:
            return in.just_index_table_scalar_per_fragment;
        case just_index_setup_kind::simd:
            return in.just_index_table_simd_per_fragment;
        case just_index_setup_kind::gpu:
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
        const e2e_input& in,
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

void capture_hash_major_build_stats(
        const it::connection_test_context& c,
        e2e_output& out) {
    const auto rows = it::query_rows(c,
        "SELECT backend_pid, db_oid, relfilenode, sequence_rowno, "
        "total_run_count, resident_runs, file_runs, resident_run_bytes, "
        "resident_peak_bytes, spilled_run_bytes, postgres_temp_file_bytes, "
        "postgres_temp_file_count, merge_reader_buffer_bytes, merge_pass_count, "
        "spill_count_from_pressure, resident_demotions, position_spill_bytes, "
        "position_spill_count, generation, worker_build_ns, pipeline_mode, "
        "max_concurrency, index_memory_cap_mb, "
        "shareable_lazy_workers AS lazy_worker_shareable "
        "FROM bioseq.scheduler_minimizer_sequence_build_stats();");
    if (rows.empty()) {
        return;
    }
    const it::row_t& row = rows.front();
    out.hash_major_build_backend_pid = row_i64_or_minus_one(row, 0);
    out.hash_major_build_db_oid = row_i64_or_minus_one(row, 1);
    out.hash_major_build_relfilenode = row_i64_or_minus_one(row, 2);
    out.hash_major_build_sequence_rowno = row_i64_or_minus_one(row, 3);
    out.hash_major_build_total_run_count = row_i64_or_minus_one(row, 4);
    out.hash_major_build_resident_runs = row_i64_or_minus_one(row, 5);
    out.hash_major_build_file_runs = row_i64_or_minus_one(row, 6);
    out.hash_major_build_resident_run_bytes = row_i64_or_minus_one(row, 7);
    out.hash_major_build_resident_peak_bytes = row_i64_or_minus_one(row, 8);
    out.hash_major_build_spilled_run_bytes = row_i64_or_minus_one(row, 9);
    out.hash_major_build_postgres_temp_file_bytes = row_i64_or_minus_one(row, 10);
    out.hash_major_build_postgres_temp_file_count = row_i64_or_minus_one(row, 11);
    out.hash_major_build_merge_reader_buffer_bytes = row_i64_or_minus_one(row, 12);
    out.hash_major_build_merge_pass_count = row_i64_or_minus_one(row, 13);
    out.hash_major_build_spill_count_from_pressure = row_i64_or_minus_one(row, 14);
    out.hash_major_build_resident_demotions = row_i64_or_minus_one(row, 15);
    out.hash_major_build_position_spill_bytes = row_i64_or_minus_one(row, 16);
    out.hash_major_build_position_spill_count = row_i64_or_minus_one(row, 17);
    out.hash_major_build_shared_generation = row_i64_or_minus_one(row, 18);
    out.hash_major_build_worker_build_ns = row_i64_or_minus_one(row, 19);
    out.hash_major_build_pipeline_mode = row_i64_or_minus_one(row, 20);
    out.hash_major_build_max_concurrency = row_i64_or_minus_one(row, 21);
    out.hash_major_build_index_memory_cap_mb = row_i64_or_minus_one(row, 22);
    out.hash_major_build_lazy_worker_shareable = row_i64_or_minus_one(row, 23);
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

[[nodiscard]] cold_no_index_policy_absence_stats
read_cold_no_index_policy_absence_stats(
        const it::connection_test_context& c,
        const std::string& table,
        const std::string_view index_name) {
    const auto rows = it::query_rows(c, fmt::format(
        "SELECT target_rows, is_target_cardinality_metadata_only::int, "
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
    return cold_no_index_policy_absence_stats{
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

[[nodiscard]] constexpr bool exact_cold_no_index_absence(
        const cold_no_index_policy_absence_stats& stats,
        const std::int64_t expected_target_rows) noexcept {
    return stats.target_rows == expected_target_rows
        && stats.target_cardinality_metadata_only == 1
        && stats.source_parent_rows_read == 0
        && stats.source_fragment_rows_read == 0
        && stats.source_decoded_bp == 0
        && stats.physical_index_relations == 0
        && stats.persisted_index_rows == 0
        && stats.generation_objects == 0;
}

void create_algorithm_matrix_query_fixture(e2e_input& in) {
    if (in.matrix_query_table.empty()) return;
    it::exec_sqlf(
        *in.conn, "DROP TABLE IF EXISTS {} CASCADE;", in.matrix_query_table);
    captured_query_rows created = query_rows_capturing_error(
        *in.conn,
        fmt::format(
            "CREATE TABLE {} (id integer NOT NULL, seq {}) USING bioseq;",
            in.matrix_query_table, in.seq_type));
    if (!is_exact_command_ok(created)) {
        if (is_allowlisted_algorithm_matrix_table_setup_rejection(created, in)) {
            // Every matrix target carrying the same unsupported typmod already
            // owns the canonical rejected_setup evidence.  The query fixture
            // must not introduce a second rejection identity.
            in.matrix_query_table.clear();
            return;
        }
        abort_unexpected_e2e_table_setup_result(created, in.matrix_query_table);
    }
    disable_e2e_autovacuum(*in.conn, in.matrix_query_table);
    it::exec_sql(
        *in.conn, "SET bioseq.index.insert_maintenance.enabled = off;");
    it::exec_sql(
        *in.conn, "SET bioseq.index.async_persistence.enabled = off;");
    it::exec_sql(
        *in.conn,
        "SET bioseq.index.recompute_async_persistence.enabled = off;");
    it::exec_sql(
        *in.conn, "SET bioseq.index.sequence_read.enabled = off;");
    for (const std::string_view index_name : {
             k_minimizers_per_fragment_index,
             k_minimizers_per_sequence_index}) {
        it::exec_sqlf(
            *in.conn,
            "SELECT bioseq.set_index_policy('{}'::regclass, '{}', "
            "'UPDATE_MANUALLY');",
            in.matrix_query_table, index_name);
    }
    for (const e2e_query_truth& query : in.queries) {
        it::exec_sqlf(
            *in.conn,
            "INSERT INTO {} (id, seq) VALUES ({}, '{}'::{});",
            in.matrix_query_table, query.id, query.bases, in.seq_type);
    }
    for (const std::string_view index_name : {
             k_minimizers_per_fragment_index,
             k_minimizers_per_sequence_index}) {
        const cold_no_index_policy_absence_stats absence =
            read_cold_no_index_policy_absence_stats(
                *in.conn, in.matrix_query_table, index_name);
        if (!exact_cold_no_index_absence(
                absence, static_cast<std::int64_t>(in.queries.size()))) {
            std::fprintf(
                stderr,
                "[bench_e2e] MATRIX-QUERY-FIXTURE-INDEX-FAIL table=%s "
                "index=%.*s rows=%lld physical=%lld persisted=%lld "
                "generations=%lld\n",
                in.matrix_query_table.c_str(),
                static_cast<int>(index_name.size()), index_name.data(),
                static_cast<long long>(absence.target_rows),
                static_cast<long long>(absence.physical_index_relations),
                static_cast<long long>(absence.persisted_index_rows),
                static_cast<long long>(absence.generation_objects));
            std::fflush(stderr);
            std::abort();
        }
    }
    const it::row_t identity = it::query_one(
        *in.conn,
        fmt::format(
            "SELECT count(*)::bigint, count(DISTINCT id)::bigint, "
            "min(id)::bigint, max(id)::bigint, am.amname "
            "FROM {} q CROSS JOIN LATERAL ("
            "SELECT a.amname FROM pg_class c JOIN pg_am a ON a.oid=c.relam "
            "WHERE c.oid='{}'::regclass) am GROUP BY am.amname;",
            in.matrix_query_table, in.matrix_query_table));
    const std::int64_t query_count = static_cast<std::int64_t>(in.queries.size());
    if (identity.size() != 5u
        || row_i64_or_minus_one(identity, 0) != query_count
        || row_i64_or_minus_one(identity, 1) != query_count
        || row_i64_or_minus_one(identity, 2) != 0
        || row_i64_or_minus_one(identity, 3) != query_count - 1
        || !identity[4].has_value() || *identity[4] != "bioseq") {
        std::fputs(
            "[bench_e2e] MATRIX-QUERY-FIXTURE-SHAPE-FAIL durable USING "
            "bioseq query identities are not exactly 0..N-1\n",
            stderr);
        std::fflush(stderr);
        std::abort();
    }
}

[[nodiscard]] std::int64_t cold_no_index_gucs_are_off(
        const it::connection_test_context& c) {
    const auto rows = it::query_rows(
        c,
        "SELECT (current_setting('bioseq.index.insert_maintenance.enabled') = 'off')::int, "
        "       (current_setting('bioseq.index.sequence_read.enabled') = 'off')::int, "
        "       (current_setting('bioseq.index.async_persistence.enabled') = 'off')::int, "
        "       (current_setting("
        "'bioseq.index.recompute_async_persistence.enabled') = 'off')::int;");
    if (rows.size() != 1 || rows.front().size() != 4) {
        return 0;
    }
    return row_i64_or_minus_one(rows.front(), 0) == 1
            && row_i64_or_minus_one(rows.front(), 1) == 1
            && row_i64_or_minus_one(rows.front(), 2) == 1
            && row_i64_or_minus_one(rows.front(), 3) == 1
        ? 1 : 0;
}

/** @brief Lifecycle point represented by a cold no-index audit. */
enum class cold_no_index_audit_stage : std::uint8_t {
    pre_restart,
    post_restart,
    measured,
};

void capture_cold_no_index_absence(
        const it::connection_test_context& c,
        const e2e_input& in,
        e2e_output& out,
        const cold_no_index_audit_stage stage) {
    const cold_no_index_policy_absence_stats minimizer_fragment =
        read_cold_no_index_policy_absence_stats(
            c, in.no_index_table, k_minimizers_per_fragment_index);
    const cold_no_index_policy_absence_stats minimizer_sequence =
        read_cold_no_index_policy_absence_stats(
            c, in.no_index_table, k_minimizers_per_sequence_index);
    const std::int64_t target_rows = minimizer_fragment.target_rows;
    const std::int64_t minimizer_fragment_rows =
        minimizer_fragment.persisted_index_rows;
    const std::int64_t minimizer_sequence_rows =
        minimizer_sequence.persisted_index_rows;
    const std::int64_t async_jobs = first_i64_or_minus_one(it::query_rows(
        c,
        "SELECT async_persist_jobs_waiting FROM bioseq.scheduler_stats();"));
    const std::int64_t gucs_off = cold_no_index_gucs_are_off(c);

    if (stage == cold_no_index_audit_stage::pre_restart) {
        out.cold_no_index_pre_restart_target_rows = target_rows;
        out.cold_no_index_pre_restart_minimizer_fragment_rows =
            minimizer_fragment_rows;
        out.cold_no_index_pre_restart_minimizer_sequence_rows =
            minimizer_sequence_rows;
        out.cold_no_index_pre_restart_async_jobs = async_jobs;
        out.cold_no_index_pre_restart_gucs_off = gucs_off;
        out.cold_no_index_pre_restart_minimizer_fragment = minimizer_fragment;
        out.cold_no_index_pre_restart_minimizer_sequence = minimizer_sequence;
    } else if (stage == cold_no_index_audit_stage::post_restart) {
        out.cold_no_index_post_restart_target_rows = target_rows;
        out.cold_no_index_post_restart_minimizer_fragment_rows =
            minimizer_fragment_rows;
        out.cold_no_index_post_restart_minimizer_sequence_rows =
            minimizer_sequence_rows;
        out.cold_no_index_post_restart_async_jobs = async_jobs;
        out.cold_no_index_post_restart_gucs_off = gucs_off;
        out.cold_no_index_post_restart_minimizer_fragment = minimizer_fragment;
        out.cold_no_index_post_restart_minimizer_sequence = minimizer_sequence;
    } else {
        out.cold_no_index_measured_async_jobs = async_jobs;
        out.cold_no_index_measured_gucs_off = gucs_off;
        out.cold_no_index_measured_minimizer_fragment = minimizer_fragment;
        out.cold_no_index_measured_minimizer_sequence = minimizer_sequence;
    }

    if (!exact_cold_no_index_absence(minimizer_fragment, 1)
        || !exact_cold_no_index_absence(minimizer_sequence, 1)
        || target_rows != minimizer_sequence.target_rows
        || minimizer_fragment_rows != 0 || minimizer_sequence_rows != 0
        || async_jobs != 0 || gucs_off != 1) {
        std::fprintf(
            stderr,
            "[bench_e2e] COLD-NO-INDEX-ABSENCE-FAIL stage=%s "
            "target_rows=%lld minimizer_fragment_rows=%lld "
            "minimizer_sequence_rows=%lld "
            "async_jobs=%lld gucs_off=%lld "
            "minimizer_fragment_meta=%lld "
            "minimizer_fragment_source=%lld/%lld/%lld "
            "minimizer_fragment_objects=%lld/%lld/%lld "
            "minimizer_sequence_meta=%lld "
            "minimizer_sequence_source=%lld/%lld/%lld "
            "minimizer_sequence_objects=%lld/%lld/%lld\n",
            stage == cold_no_index_audit_stage::pre_restart ? "pre_restart"
                : stage == cold_no_index_audit_stage::post_restart
                    ? "post_restart" : "measured",
            static_cast<long long>(target_rows),
            static_cast<long long>(minimizer_fragment_rows),
            static_cast<long long>(minimizer_sequence_rows),
            static_cast<long long>(async_jobs),
            static_cast<long long>(gucs_off),
            static_cast<long long>(
                minimizer_fragment.target_cardinality_metadata_only),
            static_cast<long long>(minimizer_fragment.source_parent_rows_read),
            static_cast<long long>(minimizer_fragment.source_fragment_rows_read),
            static_cast<long long>(minimizer_fragment.source_decoded_bp),
            static_cast<long long>(minimizer_fragment.physical_index_relations),
            static_cast<long long>(minimizer_fragment.persisted_index_rows),
            static_cast<long long>(minimizer_fragment.generation_objects),
            static_cast<long long>(
                minimizer_sequence.target_cardinality_metadata_only),
            static_cast<long long>(minimizer_sequence.source_parent_rows_read),
            static_cast<long long>(minimizer_sequence.source_fragment_rows_read),
            static_cast<long long>(minimizer_sequence.source_decoded_bp),
            static_cast<long long>(minimizer_sequence.physical_index_relations),
            static_cast<long long>(minimizer_sequence.persisted_index_rows),
            static_cast<long long>(minimizer_sequence.generation_objects));
        std::fflush(stderr);
        std::abort();
    }
}

/** @brief Inputs to the cold no-index lifecycle acceptance predicate. */
struct cold_no_index_lifecycle_gate_input {
    std::int64_t restart_performed = 0;
    std::int64_t restart_identity_changed = 0;
    std::int64_t identity_fields_present = 0;
    std::int64_t alignment_uses_post_restart_backend = 0;
    std::int64_t pre_target_rows = 0;
    std::int64_t post_target_rows = 0;
    std::int64_t pre_minimizer_fragment_rows = 0;
    std::int64_t pre_minimizer_sequence_rows = 0;
    std::int64_t post_minimizer_fragment_rows = 0;
    std::int64_t post_minimizer_sequence_rows = 0;
    std::int64_t measured_minimizer_fragment_rows = 0;
    std::int64_t measured_minimizer_sequence_rows = 0;
    std::int64_t pre_async_jobs = 0;
    std::int64_t post_async_jobs = 0;
    std::int64_t measured_async_jobs = 0;
    std::int64_t pre_gucs_off = 0;
    std::int64_t post_gucs_off = 0;
    std::int64_t measured_gucs_off = 0;
    std::int64_t metadata_only_physical_absence_validated = 0;
};

[[nodiscard]] constexpr bool cold_no_index_lifecycle_gate(
        const cold_no_index_lifecycle_gate_input& stats) noexcept {
    return stats.restart_performed == 1
        && stats.restart_identity_changed == 1
        && stats.identity_fields_present == 1
        && stats.alignment_uses_post_restart_backend == 1
        && stats.pre_target_rows == 1
        && stats.post_target_rows == 1
        && stats.pre_minimizer_fragment_rows == 0
        && stats.pre_minimizer_sequence_rows == 0
        && stats.post_minimizer_fragment_rows == 0
        && stats.post_minimizer_sequence_rows == 0
        && stats.measured_minimizer_fragment_rows == 0
        && stats.measured_minimizer_sequence_rows == 0
        && stats.pre_async_jobs == 0
        && stats.post_async_jobs == 0
        && stats.measured_async_jobs == 0
        && stats.pre_gucs_off == 1
        && stats.post_gucs_off == 1
        && stats.measured_gucs_off == 1
        && stats.metadata_only_physical_absence_validated == 1;
}

constexpr cold_no_index_lifecycle_gate_input k_cold_no_index_valid_gate_fixture{
    .restart_performed = 1,
    .restart_identity_changed = 1,
    .identity_fields_present = 1,
    .alignment_uses_post_restart_backend = 1,
    .pre_target_rows = 1,
    .post_target_rows = 1,
    .pre_gucs_off = 1,
    .post_gucs_off = 1,
    .measured_gucs_off = 1,
    .metadata_only_physical_absence_validated = 1,
};
static_assert(cold_no_index_lifecycle_gate(k_cold_no_index_valid_gate_fixture));
static_assert(!cold_no_index_lifecycle_gate([] {
    auto stats = k_cold_no_index_valid_gate_fixture;
    stats.restart_performed = 0;
    return stats;
}()));
static_assert(!cold_no_index_lifecycle_gate([] {
    auto stats = k_cold_no_index_valid_gate_fixture;
    stats.restart_identity_changed = 0;
    return stats;
}()));
static_assert(!cold_no_index_lifecycle_gate([] {
    auto stats = k_cold_no_index_valid_gate_fixture;
    stats.post_minimizer_fragment_rows = 1;
    return stats;
}()));
static_assert(!cold_no_index_lifecycle_gate([] {
    auto stats = k_cold_no_index_valid_gate_fixture;
    stats.pre_minimizer_sequence_rows = 1;
    return stats;
}()));
static_assert(!cold_no_index_lifecycle_gate([] {
    auto stats = k_cold_no_index_valid_gate_fixture;
    stats.post_async_jobs = 1;
    return stats;
}()));
static_assert(!cold_no_index_lifecycle_gate([] {
    auto stats = k_cold_no_index_valid_gate_fixture;
    stats.post_gucs_off = 0;
    return stats;
}()));
static_assert(!cold_no_index_lifecycle_gate([] {
    auto stats = k_cold_no_index_valid_gate_fixture;
    stats.alignment_uses_post_restart_backend = 0;
    return stats;
}()));
static_assert(!cold_no_index_lifecycle_gate([] {
    auto stats = k_cold_no_index_valid_gate_fixture;
    stats.metadata_only_physical_absence_validated = 0;
    return stats;
}()));

void require_cold_no_index_lifecycle(e2e_output& out) {
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
    const bool metadata_only_physical_absence_validated =
        exact_cold_no_index_absence(
            out.cold_no_index_pre_restart_minimizer_fragment, 1)
        && exact_cold_no_index_absence(
            out.cold_no_index_pre_restart_minimizer_sequence, 1)
        && exact_cold_no_index_absence(
            out.cold_no_index_post_restart_minimizer_fragment, 1)
        && exact_cold_no_index_absence(
            out.cold_no_index_post_restart_minimizer_sequence, 1)
        && exact_cold_no_index_absence(
            out.cold_no_index_measured_minimizer_fragment, 1)
        && exact_cold_no_index_absence(
            out.cold_no_index_measured_minimizer_sequence, 1);
    const cold_no_index_lifecycle_gate_input stats{
        .restart_performed = out.cold_no_index_restart_performed,
        .restart_identity_changed =
            out.cold_no_index_restart_identity_changed,
        .identity_fields_present = identity_fields_present ? 1 : 0,
        .alignment_uses_post_restart_backend =
            alignment_uses_post_restart_backend ? 1 : 0,
        .pre_target_rows = out.cold_no_index_pre_restart_target_rows,
        .post_target_rows = out.cold_no_index_post_restart_target_rows,
        .pre_minimizer_fragment_rows = out.cold_no_index_pre_restart_minimizer_fragment_rows,
        .pre_minimizer_sequence_rows = out.cold_no_index_pre_restart_minimizer_sequence_rows,
        .post_minimizer_fragment_rows = out.cold_no_index_post_restart_minimizer_fragment_rows,
        .post_minimizer_sequence_rows = out.cold_no_index_post_restart_minimizer_sequence_rows,
        .measured_minimizer_fragment_rows =
            out.cold_no_index_measured_minimizer_fragment.persisted_index_rows,
        .measured_minimizer_sequence_rows =
            out.cold_no_index_measured_minimizer_sequence.persisted_index_rows,
        .pre_async_jobs = out.cold_no_index_pre_restart_async_jobs,
        .post_async_jobs = out.cold_no_index_post_restart_async_jobs,
        .measured_async_jobs = out.cold_no_index_measured_async_jobs,
        .pre_gucs_off = out.cold_no_index_pre_restart_gucs_off,
        .post_gucs_off = out.cold_no_index_post_restart_gucs_off,
        .measured_gucs_off = out.cold_no_index_measured_gucs_off,
        .metadata_only_physical_absence_validated =
            metadata_only_physical_absence_validated ? 1 : 0,
    };
    out.cold_no_index_lifecycle_accepted =
        cold_no_index_lifecycle_gate(stats) ? 1 : 0;
    if (out.cold_no_index_lifecycle_accepted == 1) {
        return;
    }
    std::fprintf(
        stderr,
        "[bench_e2e] COLD-NO-INDEX-LIFECYCLE-FAIL restart=%lld "
        "identity_changed=%lld identity_present=%d alignment_fresh=%d "
        "target=%lld/%lld grouped_soa=%lld/%lld/%lld hash_major=%lld/%lld/%lld "
        "async=%lld/%lld/%lld gucs=%lld/%lld/%lld physical_absence=%d\n",
        static_cast<long long>(out.cold_no_index_restart_performed),
        static_cast<long long>(out.cold_no_index_restart_identity_changed),
        identity_fields_present ? 1 : 0,
        alignment_uses_post_restart_backend ? 1 : 0,
        static_cast<long long>(out.cold_no_index_pre_restart_target_rows),
        static_cast<long long>(out.cold_no_index_post_restart_target_rows),
        static_cast<long long>(out.cold_no_index_pre_restart_minimizer_fragment_rows),
        static_cast<long long>(out.cold_no_index_post_restart_minimizer_fragment_rows),
        static_cast<long long>(
            out.cold_no_index_measured_minimizer_fragment.persisted_index_rows),
        static_cast<long long>(out.cold_no_index_pre_restart_minimizer_sequence_rows),
        static_cast<long long>(out.cold_no_index_post_restart_minimizer_sequence_rows),
        static_cast<long long>(
            out.cold_no_index_measured_minimizer_sequence.persisted_index_rows),
        static_cast<long long>(out.cold_no_index_pre_restart_async_jobs),
        static_cast<long long>(out.cold_no_index_post_restart_async_jobs),
        static_cast<long long>(out.cold_no_index_measured_async_jobs),
        static_cast<long long>(out.cold_no_index_pre_restart_gucs_off),
        static_cast<long long>(out.cold_no_index_post_restart_gucs_off),
        static_cast<long long>(out.cold_no_index_measured_gucs_off),
        metadata_only_physical_absence_validated ? 1 : 0);
    std::fflush(stderr);
    std::abort();
}

void capture_index_audit(
        const it::connection_test_context& c,
        const std::string& table,
        std::string_view minimizer_index,
        e2e_output& out) {
    out.audit_selected_minimizer_is_sequence =
        minimizer_index == k_minimizers_per_sequence_index ? 1 : 0;
    out.audit_minimizer_fragment_rows =
        index_row_count(c, table, k_minimizers_per_fragment_index);
    out.audit_minimizer_sequence_rows =
        index_row_count(c, table, k_minimizers_per_sequence_index);

    out.audit_minimizer_fragment_update_on_insert = -1;
    out.audit_minimizer_sequence_update_on_insert = -1;
    out.audit_minimizer_fragment_active_read = -1;
    out.audit_minimizer_sequence_active_read = -1;
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
        if (name == k_minimizers_per_fragment_index) {
            out.audit_minimizer_fragment_update_on_insert = uoi;
            out.audit_minimizer_fragment_active_read = active;
        } else if (name == k_minimizers_per_sequence_index) {
            out.audit_minimizer_sequence_update_on_insert = uoi;
            out.audit_minimizer_sequence_active_read = active;
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
        e2e_output& out) {
    const bool expect_sequence =
        minimizer_index == k_minimizers_per_sequence_index;
    const bool minimizer_fragment_selected = !expect_sequence;
    const bool accepted =
        out.audit_selected_minimizer_is_sequence
            == (expect_sequence ? 1 : 0)
        && out.audit_minimizer_fragment_update_on_insert
            == (minimizer_fragment_selected ? 1 : 0)
        && out.audit_minimizer_sequence_update_on_insert
            == (expect_sequence ? 1 : 0)
        && out.audit_minimizer_fragment_active_read
            == (minimizer_fragment_selected ? 1 : 0)
        && out.audit_minimizer_sequence_active_read
            == (expect_sequence ? 1 : 0)
        && (minimizer_fragment_selected
            ? out.audit_minimizer_fragment_rows > 0
            : out.audit_minimizer_fragment_rows == 0)
        && (expect_sequence
            ? out.audit_minimizer_sequence_rows > 0
            : out.audit_minimizer_sequence_rows == 0)
        && out.audit_async_persist_jobs_waiting == 0
        && out.audit_index_rows_wait_attempts == 0;
    out.persisted_policy_audit_accepted = accepted ? 1 : 0;
    if (!accepted) {
        bioseq_log<logging_level::error>(
            "[bench_e2e] post-measure persisted-policy audit failed "
            "index={} selected_is_sequence={} minimizer_fragment_rows={} "
            "minimizer_sequence_rows={} minimizer_fragment_update={} "
            "minimizer_sequence_update={} minimizer_fragment_active={} "
            "minimizer_sequence_active={} "
            "async_waiting={} wait_attempts={}",
            minimizer_index,
            out.audit_selected_minimizer_is_sequence,
            out.audit_minimizer_fragment_rows,
            out.audit_minimizer_sequence_rows,
            out.audit_minimizer_fragment_update_on_insert,
            out.audit_minimizer_sequence_update_on_insert,
            out.audit_minimizer_fragment_active_read,
            out.audit_minimizer_sequence_active_read,
            out.audit_async_persist_jobs_waiting,
            out.audit_index_rows_wait_attempts);
    }
}

void capture_async_persist_waiting(
        const it::connection_test_context& c,
        e2e_output& out) {
    const auto stats = it::query_rows(c,
        "SELECT async_persist_jobs_waiting FROM bioseq.scheduler_stats();");
    out.audit_async_persist_jobs_waiting = first_i64_or_minus_one(stats);
}

void capture_index_audit_after_async_persist(
        const it::connection_test_context& c,
        const std::string& table,
        std::string_view minimizer_index,
        e2e_output& out) {
    constexpr int k_fragment_max_attempts = 180;
    constexpr int k_sequence_max_attempts = 900;
    constexpr auto k_sleep = std::chrono::seconds{1};
    const bool expect_sequence = minimizer_index == k_minimizers_per_sequence_index;
    const int max_attempts = expect_sequence ? k_sequence_max_attempts : k_fragment_max_attempts;
    for (int attempt = 0; attempt <= max_attempts; ++attempt) {
        capture_async_persist_waiting(c, out);
        out.audit_index_rows_wait_attempts = attempt;
        if (out.audit_async_persist_jobs_waiting > 0) {
            std::this_thread::sleep_for(k_sleep);
            continue;
        }
        capture_index_audit(c, table, minimizer_index, out);
        const std::int64_t target_rows =
            expect_sequence ? out.audit_minimizer_sequence_rows : out.audit_minimizer_fragment_rows;
        if (target_rows > 0 || out.audit_async_persist_jobs_waiting == 0) {
            return;
        }
        std::this_thread::sleep_for(k_sleep);
    }
}

void attach_resource_metrics(e2e_output& out,
                             std::uint64_t start_ns,
                             std::uint64_t end_ns,
                             const resource_sample& resources) {
    if (end_ns < start_ns) {
        throw std::runtime_error("manual measured-region clock moved backwards");
    }
    out.measured_region_ns = end_ns - start_ns;
    out.measured_region_available = true;
    out.peak_postgres_rss_kb = resources.postgres_rss_kb;
    out.peak_minimap2_rss_kb = resources.minimap2_rss_kb;
    out.peak_bench_rss_kb = resources.bench_rss_kb;
    out.min_tmp_available_kb = resources.tmp_available_kb;
    out.peak_swap_used_mb = resources.swap_used_mb;
}

void attach_minimap2_receipt(
        e2e_output& out,
        const minimap2_helper_result& helper) {
    const auto checked_i64 = [](const std::uint64_t value,
                                const std::string_view label) {
        if (value > static_cast<std::uint64_t>(
                (std::numeric_limits<std::int64_t>::max)())) {
            throw std::runtime_error(fmt::format(
                "validated minimap2 receipt {} exceeds int64", label));
        }
        return static_cast<std::int64_t>(value);
    };
    out.minimap2_receipt_available = 1;
    out.minimap2_receipt_pid =
        checked_i64(helper.receipt.child_pid, "pid");
    out.minimap2_cigar_interval_count = checked_i64(
        helper.receipt.cigar_interval_count, "CIGAR interval count");
    out.minimap2_mapq_interval_count = checked_i64(
        helper.receipt.mapq_interval_count, "MAPQ interval count");
    out.minimap2_cigar_union_ns = checked_i64(
        helper.receipt.cigar_union_ns, "CIGAR union");
    out.minimap2_mapq_union_ns = checked_i64(
        helper.receipt.mapq_union_ns, "MAPQ union");
    out.minimap2_cigar_or_mapq_union_ns = checked_i64(
        helper.receipt.combined_union_ns, "combined interval union");
    out.minimap2_receipt_checksum = helper.receipt.checksum;
    out.minimap2_receipt_artifact_path = helper.artifact_path;
    out.minimap2_receipt_artifact_digest = helper.artifact_digest;
    out.minimap2_receipt_summary = helper.receipt.summary;
}

/** Seal one logical minimap2 sample from independently bounded helpers. */
void attach_logical_minimap2_receipt(
        const e2e_input& in,
        e2e_output& out,
        const std::vector<logical_minimap2_chunk_result>& chunks) {
    const auto plan = make_logical_query_chunks(
        in.queries.size(), in.logical_query_chunk_size);
    if (chunks.size() != plan.size() || chunks.empty()) {
        throw std::runtime_error(
            "logical minimap2 receipt does not cover its deterministic plan");
    }
    const auto add_u64 = [](const std::uint64_t lhs,
                            const std::uint64_t rhs,
                            const std::string_view label) {
        if (rhs > (std::numeric_limits<std::uint64_t>::max)() - lhs) {
            throw std::runtime_error(fmt::format(
                "logical minimap2 {} overflow", label));
        }
        return lhs + rhs;
    };
    std::uint64_t measured_sum = 0;
    std::uint64_t cigar_count = 0;
    std::uint64_t mapq_count = 0;
    std::uint64_t cigar_union = 0;
    std::uint64_t mapq_union = 0;
    std::uint64_t combined_union = 0;
    nlohmann::json physical = nlohmann::json::array();
    for (std::size_t index = 0; index < chunks.size(); ++index) {
        const auto& chunk = chunks[index];
        const auto& expected = plan[index];
        const auto& helper = chunk.helper;
        if (chunk.query_begin != expected.begin
            || chunk.query_end != expected.end
            || helper.receipt.child_pid == 0
            || helper.receipt.combined_union_ns > chunk.measured_ns
            || helper.artifact_path.empty()
            || !fixed_lower_hex(helper.artifact_digest, 16)
            || !fixed_lower_hex(helper.receipt.checksum, 16)) {
            throw std::runtime_error(
                "logical minimap2 chunk receipt is incomplete or out of order");
        }
        measured_sum = add_u64(
            measured_sum, chunk.measured_ns, "measured-duration sum");
        cigar_count = add_u64(
            cigar_count, helper.receipt.cigar_interval_count,
            "CIGAR interval-count sum");
        mapq_count = add_u64(
            mapq_count, helper.receipt.mapq_interval_count,
            "MAPQ interval-count sum");
        cigar_union = add_u64(
            cigar_union, helper.receipt.cigar_union_ns,
            "CIGAR union sum");
        mapq_union = add_u64(
            mapq_union, helper.receipt.mapq_union_ns,
            "MAPQ union sum");
        combined_union = add_u64(
            combined_union, helper.receipt.combined_union_ns,
            "combined union sum");
        physical.push_back(nlohmann::json{
            {"ordinal", index},
            {"query_begin", chunk.query_begin},
            {"query_end", chunk.query_end},
            {"query_count", chunk.query_end - chunk.query_begin},
            {"measured_algorithm_ns", chunk.measured_ns},
            {"cigar_interval_count", helper.receipt.cigar_interval_count},
            {"mapq_interval_count", helper.receipt.mapq_interval_count},
            {"cigar_union_ns", helper.receipt.cigar_union_ns},
            {"mapq_union_ns", helper.receipt.mapq_union_ns},
            {"combined_union_ns", helper.receipt.combined_union_ns},
            {"child_pid", helper.receipt.child_pid},
            {"receipt_checksum", helper.receipt.checksum},
            {"receipt_artifact", nlohmann::json{
                {"path", helper.artifact_path},
                {"digest_algorithm", "fnv-1a-64_raw_receipt_bytes"},
                {"digest", helper.artifact_digest},
            }},
            {"validated_summary", helper.receipt.summary},
        });
    }
    if (combined_union > measured_sum) {
        throw std::runtime_error(
            "logical minimap2 interval union exceeds summed measured work");
    }
    nlohmann::json manifest{
        {"schema", "bioseq.e2e.logical-minimap2-chunk-receipt"},
        {"version", 1},
        {"query_count", in.queries.size()},
        {"chunk_size", in.logical_query_chunk_size},
        {"chunk_count", chunks.size()},
        {"statement_timeout_ms", in.statement_timeout_ms},
        {"timeout_scope", "one_direct_minimap2_helper_per_physical_chunk"},
        {"deterministic_order", "contiguous_global_query_id_ascending"},
        {"measured_algorithm_ns_sum", measured_sum},
        {"cigar_interval_count_sum", cigar_count},
        {"mapq_interval_count_sum", mapq_count},
        {"cigar_union_ns_sum", cigar_union},
        {"mapq_union_ns_sum", mapq_union},
        {"combined_union_ns_sum", combined_union},
        {"setup_receipt", nlohmann::json{
            {"checksum", in.minimap2_setup_receipt_checksum},
            {"artifact_path", in.minimap2_setup_receipt_artifact_path},
            {"artifact_digest", in.minimap2_setup_receipt_artifact_digest},
            {"validated_summary", in.minimap2_setup_receipt_summary},
        }},
        {"physical_chunks", std::move(physical)},
        {"raw_paf_embedded", false},
    };
    const std::string body = manifest.dump();
    minimap2_fnv1a64 digest;
    for (const unsigned char byte : body) digest.append_byte(byte);
    const std::string digest_text = minimap2_hex_u64(digest.value);
    const std::string token = unpredictable_minimap2_run_token();
    const std::string artifact_path =
        preserve_minimap2_receipt_artifact(token, body).string();
    const auto checked_i64 = [](const std::uint64_t value,
                                const std::string_view label) {
        if (value > static_cast<std::uint64_t>(
                (std::numeric_limits<std::int64_t>::max)())) {
            throw std::runtime_error(fmt::format(
                "logical minimap2 {} exceeds int64", label));
        }
        return static_cast<std::int64_t>(value);
    };
    out.measured_region_ns = measured_sum;
    out.measured_region_available = true;
    out.minimap2_receipt_available = 1;
    out.minimap2_receipt_pid = checked_i64(
        chunks.front().helper.receipt.child_pid, "first child PID");
    out.minimap2_cigar_interval_count = checked_i64(
        cigar_count, "CIGAR interval count");
    out.minimap2_mapq_interval_count = checked_i64(
        mapq_count, "MAPQ interval count");
    out.minimap2_cigar_union_ns = checked_i64(
        cigar_union, "CIGAR union");
    out.minimap2_mapq_union_ns = checked_i64(
        mapq_union, "MAPQ union");
    out.minimap2_cigar_or_mapq_union_ns = checked_i64(
        combined_union, "combined union");
    out.minimap2_receipt_checksum = digest_text;
    out.minimap2_receipt_artifact_path = artifact_path;
    out.minimap2_receipt_artifact_digest = digest_text;
    out.minimap2_receipt_summary = manifest;
    out.logical_chunk_receipts = std::move(manifest);
    out.logical_chunking_enabled = 1;
    out.logical_chunk_count = checked_i64(chunks.size(), "chunk count");
    out.logical_chunk_size = checked_i64(
        in.logical_query_chunk_size, "chunk size");
    out.logical_statement_timeout_ms = checked_i64(
        in.statement_timeout_ms, "statement timeout");
    out.logical_statement_timeout_restored = 1;
}

void require_minimap2_union_within_manual_envelope(
        const e2e_output& out) {
    if (!out.measured_region_available ||
        out.minimap2_receipt_available != 1 ||
        out.minimap2_cigar_or_mapq_union_ns < 0 ||
        static_cast<std::uint64_t>(out.minimap2_cigar_or_mapq_union_ns) >
            out.measured_region_ns) {
        throw std::runtime_error(
            "validated minimap2 interval union exceeds its same-run envelope");
    }
}

void log_measured_case(std::string_view algo,
                       std::string_view pipeline_mode,
                       int minimap_threads,
                       const e2e_input& in,
                       const e2e_output& out,
                       std::string_view minimizer_index = "-") {
    bioseq_log<logging_level::debug>("[bench_e2e] measured algo={} pipeline_mode={} minimap_threads={} minimizer_index={} size={} measured_ns={} pg_peak_rss_mb={:.2f} minimap2_peak_rss_mb={:.2f} bench_peak_rss_mb={:.2f} tmp_min_free_gib={:.2f} swap_peak_mb={:.2f} found={} offset_bp={}",
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
    bioseq_log<logging_level::debug>(
            "[bench_e2e] validation_env workers={}/{} lifecycle=(nodes={},nested={},completed={},partial={},drained={}) persisted=(generation={},source={}) gpu=(uuid={},driver={},cache_policy={},seed_sha256={},seed_size={},manifest={},hit={})",
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
                       const e2e_input& in) {
    const char* keep_artifacts_env = std::getenv("BIOSEQ_E2E_KEEP_ARTIFACTS");
    const bool keep_artifacts = jbool(
        data, "keep_benchmark_artifacts", false)
        || (keep_artifacts_env != nullptr
            && std::string_view{keep_artifacts_env} == "1");
    if (keep_artifacts) {
        bioseq_log<logging_level::debug>("[bench_e2e] keeping SQL tables and FASTA/MMI artifacts "
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
        for (const auto& [identity, table] : in.matrix_just_index_tables) {
            (void)identity;
            if (table == in.matrix_just_index_target_to_retain
                && in.just_index_sequence_rowno_by_table.contains(table)) {
                continue;
            }
            it::exec_sqlf(*in.conn, "DROP TABLE IF EXISTS {} CASCADE;", table);
        }
        it::exec_sqlf(*in.conn, "DROP TABLE IF EXISTS {} CASCADE;", in.no_index_table);
        if (!in.matrix_query_table.empty()) {
            it::exec_sqlf(
                *in.conn, "DROP TABLE IF EXISTS {} CASCADE;",
                in.matrix_query_table);
        }
    }

    for (const auto& path : {in.target_fasta, in.target_fasta_split,
                             in.query_fasta, in.mmi_path}) {
        std::error_code ec;
        std::filesystem::remove(path, ec);
        if (ec) {
            bioseq_log<logging_level::warning>("[bench_e2e] cleanup could not remove '{}': {}", path, ec.message());
        }
    }
    for (const std::string& path : in.query_chunk_fastas) {
        std::error_code error;
        std::filesystem::remove(path, error);
        if (error) {
            bioseq_log<logging_level::warning>(
                "[bench_e2e] cleanup could not remove '{}': {}",
                path, error.message());
        }
    }
}

// ---- §10.1 on_estimate ----
template <sequence_alphabet p_alphabet>
[[nodiscard]] run_constraints e2e_estimate_typed(simdjson::dom::element data) {
    using traits = compact_sequence_range<bench_config<p_alphabet>>::traits;
    using storage_unit_t = typename bench_config<p_alphabet>::storage_unit;
    const auto ref_bp = static_cast<std::size_t>(
        jint(data, "reference_length", k_default_reference_length_bp));
    const auto query_bp = static_cast<std::size_t>(
        jint(data, "fragment_length_mean", 5'000'000));
    const std::size_t packed_units =
        (ref_bp + traits::letters_per_unit - 1) / traits::letters_per_unit;
    const std::size_t packed_bytes = packed_units * sizeof(storage_unit_t);
    const std::size_t unpacked_reference_bytes =
        ref_bp * sizeof(p_alphabet);
    const std::size_t query_bytes = query_bp * sizeof(p_alphabet);
    return {
        .base_memory = packed_bytes + query_bytes,
        .extra_memory = unpacked_reference_bytes + query_bytes
    };
}

[[nodiscard]] run_constraints e2e_estimate(std::string_view, std::string_view,
                                          simdjson::dom::element data) {
    run_constraints out{};
    dispatch_bench_alphabet(jstr(data, "alphabet", "dna4"),
        [&out, data]<sequence_alphabet p_alphabet>() {
            out = e2e_estimate_typed<p_alphabet>(data);
        });
    return out;
}

inline constexpr int k_normal_user_best_nice = 0;

/**
 * @brief Establish and read back the best prompt-free normal-user priority.
 *
 * Normal users cannot request a negative nice value on the campaign host.
 * This opt-in call therefore requests NI=0 exactly. It runs before any timed
 * region and addresses one exact PID; no polling or periodic sampling occurs.
 */
[[nodiscard]] std::int64_t establish_e2e_process_nice(
        const std::int64_t process_id,
        const std::string_view role) {
    if (process_id <= 0
        || static_cast<std::uint64_t>(process_id)
            > static_cast<std::uint64_t>(
                (std::numeric_limits<id_t>::max)())) {
        std::fprintf(
            stderr,
            "[bench_e2e] PRIORITY-ATTESTATION-FATAL role=%.*s invalid_pid=%lld\n",
            static_cast<int>(role.size()), role.data(),
            static_cast<long long>(process_id));
        std::fflush(stderr);
        std::abort();
    }
    const auto exact_process = static_cast<id_t>(process_id);
    errno = 0;
    if (::setpriority(PRIO_PROCESS, exact_process, k_normal_user_best_nice)
        != 0) {
        const int saved_errno = errno;
        std::fprintf(
            stderr,
            "[bench_e2e] PRIORITY-ATTESTATION-FATAL role=%.*s pid=%lld "
            "setpriority_target=0 errno=%d\n",
            static_cast<int>(role.size()), role.data(),
            static_cast<long long>(process_id), saved_errno);
        std::fflush(stderr);
        std::abort();
    }
    errno = 0;
    const int observed = ::getpriority(PRIO_PROCESS, exact_process);
    const int saved_errno = errno;
    if (saved_errno != 0 || observed != k_normal_user_best_nice) {
        std::fprintf(
            stderr,
            "[bench_e2e] PRIORITY-ATTESTATION-FATAL role=%.*s pid=%lld "
            "target=0 observed=%d errno=%d\n",
            static_cast<int>(role.size()), role.data(),
            static_cast<long long>(process_id), observed, saved_errno);
        std::fflush(stderr);
        std::abort();
    }
    return static_cast<std::int64_t>(observed);
}

[[nodiscard]] bool valid_e2e_artifact_label(
        const std::string_view label) noexcept {
    if (label.empty() || label.size() > 128u) return false;
    return std::ranges::all_of(label, [](const char value) noexcept {
        return (value >= 'a' && value <= 'z')
            || (value >= 'A' && value <= 'Z')
            || (value >= '0' && value <= '9')
            || value == '_' || value == '-';
    });
}

// ---- §10.2 on_input (all untimed setup) ----
template <sequence_alphabet p_alphabet>
[[nodiscard]] e2e_input e2e_input_typed(std::string_view /*data_dir*/,
                                       std::string_view test_case_name,
                                       simdjson::dom::element data,
                                       const run_constraints& constraints) {
    gen::generator_params gp{};
    gp.reference_length       = static_cast<std::size_t>(jint(data, "reference_length", k_default_reference_length_bp));
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

    e2e_input in;
    in.alphabet = std::string{bench_alphabet_name<p_alphabet>()};
    in.seq_type = bench_seq_type<p_alphabet>();
    in.size_label  = jstr(data, "size_label", "x");
    in.kmer_size   = static_cast<int>(jint(data, "kmer_size", k_default_kmer_size));
    in.window_size = static_cast<int>(jint(data, "window_size", k_default_window_size));
    in.cigar_projection = jbool(data, "cigar_projection", false);
    in.mapq_projection = jbool(data, "mapq_projection", false);
    in.prefilter_enabled = jbool(data, "prefilter_enabled", false);
    in.paired_minimap2_oracle = jbool(
        data, "paired_minimap2_oracle", false);
    const std::int64_t configured_chunk_size = jint(
        data, "logical_query_chunk_size", 0);
    const std::int64_t configured_statement_timeout_ms = jint(
        data, "statement_timeout_ms", 0);
    const bool chunk_plan_present = configured_chunk_size != 0
        || configured_statement_timeout_ms != 0;
    if (chunk_plan_present != in.paired_minimap2_oracle
        || (configured_chunk_size == 0)
            != (configured_statement_timeout_ms == 0)
        || configured_chunk_size < 0
        || configured_statement_timeout_ms < 0
        || (chunk_plan_present
            && (configured_chunk_size < 2
                || static_cast<std::size_t>(configured_chunk_size)
                    > gp.fragment_count
                || configured_statement_timeout_ms < 1'000
                || static_cast<std::uint64_t>(
                    configured_statement_timeout_ms)
                    > k_e2e_max_statement_timeout_ms))) {
        std::fprintf(
            stderr,
            "[bench_e2e] LOGICAL-CHUNK-CONFIG-FATAL chunk_size=%lld "
            "query_count=%zu statement_timeout_ms=%lld maximum_timeout_ms=%llu\n",
            static_cast<long long>(configured_chunk_size),
            gp.fragment_count,
            static_cast<long long>(configured_statement_timeout_ms),
            static_cast<unsigned long long>(
                k_e2e_max_statement_timeout_ms));
        std::fflush(stderr);
        std::abort();
    }
    if (chunk_plan_present) {
        in.logical_query_chunk_size =
            static_cast<std::size_t>(configured_chunk_size);
        in.statement_timeout_ms = static_cast<std::uint64_t>(
            configured_statement_timeout_ms);
    }
    in.require_nice_attestation = jbool(
        data, "require_nice_attestation", false);
    if (in.require_nice_attestation) {
        in.priority_target_nice = k_normal_user_best_nice;
        in.runner_pid = static_cast<std::int64_t>(::getpid());
        in.runner_nice = establish_e2e_process_nice(
            in.runner_pid, "benchmark_runner");
    }
    in.designed_indel_cigar = jbool(data, "designed_indel_cigar", false);
    in.designed_many_gap_cigar = jbool(data, "designed_many_gap_cigar", false);
    in.require_cigar_indel = jbool(data, "require_cigar_indel", false);
    in.min_cigar_deletion_ops =
        static_cast<std::uint32_t>(jint(data, "min_cigar_deletion_ops", 0));
    in.min_cigar_insertion_ops =
        static_cast<std::uint32_t>(jint(data, "min_cigar_insertion_ops", 0));
    in.minimap2_split_bp = static_cast<std::size_t>(jint(data, "minimap2_split_bp", k_minimap2_split_bp));
    const std::size_t split_bp = in.minimap2_split_bp;
    using active_traits =
        typename bench_sequence_range<p_alphabet>::traits;
    if (split_bp > static_cast<std::size_t>(k_minimap2_max_record_bp) ||
        split_bp % active_traits::letters_per_unit != 0) {
        const std::size_t lpu =
            static_cast<std::size_t>(active_traits::letters_per_unit);
        // Next value that is >= split_bp and a multiple of letters_per_unit, so the
        // error can suggest a concrete fix. (lpu is always >= 1.)
        const std::size_t suggested =
            (split_bp % lpu == 0) ? split_bp : ((split_bp / lpu) + 1u) * lpu;
        bioseq_log<logging_level::error>("[bench_e2e] minimap2_split_bp={} is invalid for alphabet {} "
             "(letters_per_unit={}, max={})",
             split_bp, in.alphabet, active_traits::letters_per_unit,
             k_minimap2_max_record_bp);
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
            static_cast<long long>(k_minimap2_max_record_bp), lpu, lpu, suggested);
        std::fputs(msg.c_str(), stderr);
        std::fflush(stderr);
        std::abort();
    }

    const std::string tmp = "/tmp";
    const std::string artifact_label = jstr(
        data, "artifact_label", in.size_label);
    if (!valid_e2e_artifact_label(artifact_label)) {
        std::fprintf(
            stderr,
            "[bench_e2e] FATAL artifact_label must contain 1..128 ASCII "
            "letters, digits, '_' or '-': '%s'\n",
            artifact_label.c_str());
        std::fflush(stderr);
        std::abort();
    }
    in.target_fasta        = fmt::format("{}/e2e_{}_target.fasta",        tmp, artifact_label);
    in.target_fasta_split  = fmt::format("{}/e2e_{}_target.split.fasta",  tmp, artifact_label);
    in.query_fasta         = fmt::format("{}/e2e_{}_query.fasta",         tmp, artifact_label);
    in.mmi_path            = fmt::format("{}/e2e_{}_target.mmi",          tmp, artifact_label);
    in.just_index_table_simd_per_fragment =
        fmt::format("e2e_{}_just_simd_minimizer_fragment", in.size_label);
    in.just_index_table_simd_per_sequence =
        fmt::format("e2e_{}_just_simd_minimizer_sequence", in.size_label);
    in.just_index_table_scalar_per_fragment =
        fmt::format("e2e_{}_just_scalar_minimizer_fragment", in.size_label);
    in.just_index_table_scalar_per_sequence =
        fmt::format("e2e_{}_just_scalar_minimizer_sequence", in.size_label);
    in.just_index_table_gpu_per_fragment =
        fmt::format("e2e_{}_just_gpu_minimizer_fragment", in.size_label);
    in.just_index_table_gpu_per_sequence =
        fmt::format("e2e_{}_just_gpu_minimizer_sequence", in.size_label);
    in.just_index_table_per_fragment = in.just_index_table_simd_per_fragment;
    in.just_index_table_per_sequence = in.just_index_table_simd_per_sequence;
    in.no_index_table = fmt::format("e2e_{}_{}_k{}_w{}_noidx",
        in.size_label, in.alphabet, in.kmer_size, in.window_size);
    const e2e_just_index_setup_requirements setup_requirements =
        resolve_e2e_just_index_setup_requirements(constraints);
    for (const auto& setup : setup_requirements.matrix_setups) {
        in.matrix_just_index_tables.emplace(
            setup.identity, algorithm_matrix_table_name(in, setup));
    }
    const bool nonpaired_algorithm_matrix_selected =
        !in.paired_minimap2_oracle
        && std::ranges::any_of(
            constraints.execution_tuples,
            [](const auto& tuple) noexcept {
                return tuple.algorithm_name ==
                        bioseq::bench::e2e::detail::algorithm_matrix_just_index
                    || tuple.algorithm_name ==
                        bioseq::bench::e2e::detail::algorithm_matrix_no_index;
            });
    if (nonpaired_algorithm_matrix_selected) {
        in.matrix_query_table = algorithm_matrix_query_table_name(
            in, test_case_name, gp.fragment_count, gp.seed);
    }

    /* Parse reuse before any multi-gigabyte artifact write.  Query truth is
     * still regenerated deterministically in memory, but a retained fixture
     * must not rewrite roughly 4 GiB of target FASTA and evict the very cold
     * index pages that the next reader-only iteration intends to measure. */
    const char* skip_index_setup_env =
        std::getenv("BIOSEQ_E2E_SKIP_INDEX_BASELINE_SETUP");
    const std::optional<bool> configured_skip_index_baseline_setup =
        jbool_optional(data, "skip_index_baseline_setup");
    const bool requested_skip_index_baseline_setup =
        configured_skip_index_baseline_setup.value_or(
            skip_index_setup_env != nullptr
            && std::string_view{skip_index_setup_env} == "1");
    bool skip_index_baseline_setup = requested_skip_index_baseline_setup;
    const char* skip_minimap2_setup_env =
        std::getenv("BIOSEQ_E2E_SKIP_MINIMAP2_BASELINE_SETUP");
    const std::optional<bool> configured_skip_minimap2_baseline_setup =
        jbool_optional(data, "skip_minimap2_baseline_setup");
    // An explicit JSON value is the run identity and therefore overrides an
    // ambient convenience variable. This lets a paired-oracle config require
    // a fresh .mmi phase while reusing only the Bioseq persisted index.
    const bool skip_minimap2_baseline_setup =
        configured_skip_minimap2_baseline_setup.value_or(
            skip_minimap2_setup_env != nullptr
            && std::string_view{skip_minimap2_setup_env} == "1");
    const char* retain_matrix_target_env =
        std::getenv("BIOSEQ_E2E_RETAIN_MATRIX_JUST_INDEX_TARGET");
    if (retain_matrix_target_env != nullptr
        && std::string_view{retain_matrix_target_env} != "0"
        && std::string_view{retain_matrix_target_env} != "1") {
        std::fprintf(
            stderr,
            "[bench_e2e] unsupported BIOSEQ_E2E_RETAIN_MATRIX_JUST_INDEX_TARGET='%s'\n",
            retain_matrix_target_env);
        std::fflush(stderr);
        std::abort();
    }
    const bool retain_matrix_just_index_target =
        retain_matrix_target_env != nullptr
        && std::string_view{retain_matrix_target_env} == "1";
    if (retain_matrix_just_index_target
        && (in.paired_minimap2_oracle
            || constraints.execution_tuples.size() != 1u
            || constraints.execution_tuples.front().algorithm_name !=
                bioseq::bench::e2e::detail::algorithm_matrix_just_index
            || setup_requirements.matrix_setups.size() != 1u
            || setup_requirements.scalar_per_fragment
            || setup_requirements.scalar_per_sequence
            || setup_requirements.simd_per_fragment
            || setup_requirements.simd_per_sequence
            || setup_requirements.gpu_per_fragment
            || setup_requirements.gpu_per_sequence
            || setup_requirements.minimap2)) {
        std::fputs(
            "[bench_e2e] matrix target retention requires one non-paired "
            "Bioseq algorithm-matrix just-index tuple\n",
            stderr);
        std::fflush(stderr);
        std::abort();
    }
    const char* just_index_setup_pipeline_env =
        std::getenv("BIOSEQ_E2E_JUST_INDEX_SETUP_PIPELINE_MODE");
    const std::string just_index_setup_pipeline_mode =
        just_index_setup_pipeline_env != nullptr
            ? std::string{just_index_setup_pipeline_env}
            : std::string{"task_parallel"};
    if (just_index_setup_pipeline_mode != "single_thread"
        && just_index_setup_pipeline_mode != "parallel_dispatch"
        && just_index_setup_pipeline_mode != "task_parallel") {
        bioseq_log<logging_level::error>("[bench_e2e] unsupported BIOSEQ_E2E_JUST_INDEX_SETUP_PIPELINE_MODE='{}'",
             just_index_setup_pipeline_mode);
        std::abort();
    }

    if (in.paired_minimap2_oracle) {
        const auto is_exact_full_vulkan_mpf = [](const auto& tuple) {
            return tuple.algorithm_name ==
                    bioseq::bench::e2e::detail::algorithm_matrix_just_index
                && param_string(tuple.params, "minimizer_algorithm", "")
                    == "vulkan_minimap_integer"
                && param_string(tuple.params, "chaining_algorithm", "")
                    == "vulkan_tiled_dynamic_programming"
                && param_string(tuple.params, "base_alignment_algorithm", "")
                    == "block_aligner"
                && param_string(tuple.params, "minimizer_index", "")
                    == k_minimizers_per_fragment_index;
        };
        const auto is_exact_minimap2_baseline = [](const auto& tuple) {
            return tuple.algorithm_name == "minimap2_just_index";
        };
        const auto full_vulkan_count = std::ranges::count_if(
            constraints.execution_tuples, is_exact_full_vulkan_mpf);
        const auto minimap2_count = std::ranges::count_if(
            constraints.execution_tuples, is_exact_minimap2_baseline);
        if (constraints.execution_tuples.size() != 2u
            || full_vulkan_count != 1 || minimap2_count != 1
            || gp.fragment_count < 2u
            || !in.cigar_projection || !in.mapq_projection
            || in.prefilter_enabled
            || !skip_index_baseline_setup
            || skip_minimap2_baseline_setup
            || !setup_requirements.minimap2
            || !chunk_plan_present) {
            std::fputs(
                "[bench_e2e] PAIRED-ORACLE-CONFIG-FATAL requires exactly "
                "one full-Vulkan MPF just-index row and one minimap2 "
                "just-index row, q>=2, CIGAR+MAPQ projection, prefilter=false, "
                "Bioseq fixture reuse, and fresh minimap2 setup\n",
                stderr);
            std::fflush(stderr);
            std::abort();
        }
    } else if (chunk_plan_present) {
        std::fputs(
            "[bench_e2e] LOGICAL-CHUNK-CONFIG-FATAL bounded execution is "
            "available only to the E2E paired minimap2 oracle contract\n",
            stderr);
        std::fflush(stderr);
        std::abort();
    }

    // (1) generate one shared target plus N query fragments.
    auto pairs = gen::alphabet_sequence_generator<
                     p_alphabet,
                     bench_storage_t>(gp)
                     .generate_sequences_pairs(gp.fragment_count);
    if (pairs.empty()) {
        std::fputs("[bench_e2e] generated zero query fragments\n", stderr);
        std::abort();
    }
    in.queries.clear();
    in.queries.reserve(pairs.size());
    for (std::size_t i = 0; i < pairs.size(); ++i) {
        const bench_pair_t<p_alphabet>& pair = pairs[i];
        in.queries.push_back(e2e_query_truth{
            .id = static_cast<std::int32_t>(i),
            .bases = pair.query.to_string(),
            .truth_target_start =
                static_cast<std::int64_t>(pair.truth_target_start),
            .truth_target_end =
                static_cast<std::int64_t>(pair.truth_target_end),
            .truth_is_reverse = pair.is_reverse,
        });
        const std::size_t target_window_start = pair.truth_target_start
            > static_cast<std::size_t>(k_position_tolerance_bp)
            ? pair.truth_target_start
                - static_cast<std::size_t>(k_position_tolerance_bp)
            : 0u;
        const std::size_t target_window_end = std::min(
            pair.target->bp_size(),
            pair.truth_target_end
                + static_cast<std::size_t>(k_position_tolerance_bp));
        in.queries.back().target_window_start = static_cast<std::int64_t>(
            target_window_start);
        in.queries.back().target_window_bases =
            decode_target_window<p_alphabet>(
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
    const bench_pair_t<p_alphabet>& first = pairs.front();
    in.truth_target_start = static_cast<std::int64_t>(first.truth_target_start);
    in.truth_is_reverse   = first.is_reverse;
    in.target_bp          = static_cast<std::int64_t>(gp.reference_length);
    std::string generated_target_content_crc64;
    if (in.paired_minimap2_oracle || retain_matrix_just_index_target) {
        generated_target_content_crc64 = minimap2_hex_u64(
            decoded_target_content_crc64<p_alphabet>(*first.target));
        if (!fixed_lower_hex(generated_target_content_crc64, 16)) {
            std::fputs(
                "[bench_e2e] generated target content checksum is malformed\n",
                stderr);
            std::fflush(stderr);
            std::abort();
        }
    }
    if (in.paired_minimap2_oracle) {
        in.paired_generated_target_content_crc64 =
            generated_target_content_crc64;
    }

    // Observed-rung JI singletons request validated reuse, including q2 retries.
    // A missing extension or exact target is a normal first setup; once the
    // target exists, every READY identity axis must match before any large
    // artifact is written.
    if (retain_matrix_just_index_target) {
        const auto& setup = setup_requirements.matrix_setups.front();
        const std::string& table = in.matrix_just_index_tables.at(setup.identity);
        in.matrix_just_index_target_to_retain = table;
        if (requested_skip_index_baseline_setup) {
            in.conn = std::make_shared<it::connection_test_context>();
            const bool extension_present = it::query_scalar_int(
                *in.conn,
                "SELECT (count(*) = 1)::int FROM pg_extension "
                "WHERE extname='bioseq';") == 1;
            bool target_present = extension_present
                && it::query_scalar_int(
                    *in.conn,
                    fmt::format(
                        "SELECT (to_regclass('{}') IS NOT NULL)::int;", table)) == 1;
            if (target_present && it::query_scalar_int(
                    *in.conn, fmt::format(
                        "SELECT EXISTS(SELECT 1 FROM {} LIMIT 1)::int;", table)) == 0) {
                // A failed/crashed FASTA load can leave its empty fixture table.
                // It is not a reusable target: use the ordinary untimed setup
                // below, which recreates this deterministic benchmark fixture.
                // Nonempty targets still require all existing READY checks.
                target_present = false;
            }
            if (target_present) {
                configure_bioseq_algorithm_matrix_session(
                    *in.conn, in.kmer_size, in.window_size,
                    setup.minimizer_algorithm,
                    setup.chaining_algorithm,
                    setup.base_alignment_algorithm,
                    just_index_setup_pipeline_mode,
                    setup.minimizer_index,
                    /*build_target=*/true);
                const std::int64_t sequence_rowno =
                    capture_only_sequence_rowno(*in.conn, table);
                skip_index_baseline_setup = validate_reusable_algorithm_matrix_target(
                    in, setup, table, sequence_rowno,
                    generated_target_content_crc64);
            } else {
                skip_index_baseline_setup = false;
            }
        }
    }

    const bool minimap2_only_singleton =
        !in.paired_minimap2_oracle
        && constraints.execution_tuples.size() == 1u
        && (constraints.execution_tuples.front().algorithm_name == "minimap2_just_index"
            || constraints.execution_tuples.front().algorithm_name == "minimap2_no_index");

    // (2) Write artifacts only while creating the fixture.  Reuse keeps the
    // existing files byte-for-byte; SQL-only cells do not consume them, while
    // minimap/no-index cells can reuse the retained deterministic copies.
    if (!skip_index_baseline_setup) {
        // Only bioseq.load consumes the unsplit copy. A standalone minimap2
        // run uses the split FASTA exclusively, including index construction.
        if (!minimap2_only_singleton) {
            write_single_record_fasta<p_alphabet>(
                in.target_fasta, "ref", *first.target);
        }
        write_split_fasta<p_alphabet>(
            in.target_fasta_split, "ref", *first.target, split_bp);
        write_multi_query_fasta<p_alphabet>(in.query_fasta, in.queries);
    } else if (setup_requirements.minimap2
               && !skip_minimap2_baseline_setup) {
        // Reuse applies only to the already-persisted Bioseq generation. A
        // paired minimap2 baseline receives a fresh, config-namespaced target,
        // query file, and .mmi so q1024 can never consume a stale q1/q8 query
        // artifact or an index built from an unproven target.
        write_split_fasta<p_alphabet>(
            in.target_fasta_split, "ref", *first.target, split_bp);
        write_multi_query_fasta<p_alphabet>(in.query_fasta, in.queries);
    }
    // All truth, query strings and required FASTA files now own their data.
    // Release the shared multi-Gbp generator target before SQL/index setup:
    // otherwise this idle buffer can occupy RAM/zram throughout a large
    // external index build. No mapping algorithm or measured interval changes.
    pairs.clear();
    if (in.paired_minimap2_oracle) {
        in.query_chunk_fastas.clear();
        const std::span<const e2e_query_truth> all_queries{in.queries};
        for (const logical_query_chunk chunk : make_logical_query_chunks(
                 in.queries.size(), in.logical_query_chunk_size)) {
            const std::string path = fmt::format(
                "{}/e2e_{}_query.chunk_{:06}_count_{:06}.fasta",
                tmp, artifact_label, chunk.begin, chunk.size());
            write_multi_query_fasta<p_alphabet>(
                path, all_queries.subspan(chunk.begin, chunk.size()));
            in.query_chunk_fastas.push_back(path);
        }
        if (in.query_chunk_fastas.empty()) {
            std::fputs(
                "[bench_e2e] LOGICAL-CHUNK-CONFIG-FATAL no query chunks "
                "were materialized\n",
                stderr);
            std::fflush(stderr);
            std::abort();
        }
    }

    /* An explicit skip is an iteration accelerator for reader-only changes:
     * retain the already-published deterministic target/generation and let
     * the same pre/post-restart READY audits below re-attest it.  Resetting the
     * extension here used to erase the very setup that the flag asked us to
     * reuse, making every 2-Gbp reader iteration rebuild for tens of minutes. */
    // (3) connect once; normally establish a clean slate.  The opt-in reuse
    // path keeps the installed extension and exact benchmark tables intact.
    if (in.conn == nullptr) {
        in.conn = std::make_shared<it::connection_test_context>();
    }
    // A standalone minimap2 baseline consumes no Bioseq target. Preserve an
    // existing extension so interleaved baselines do not erase a retained
    // matrix index; fresh extensions and all Bioseq/paired setups are unchanged.
    const bool preserve_existing_extension_for_minimap2 =
        minimap2_only_singleton
        && it::query_scalar_int(
            *in.conn,
            "SELECT count(*) FROM pg_extension WHERE extname='bioseq';") == 1;
    if (!skip_index_baseline_setup && !preserve_existing_extension_for_minimap2) {
        std::vector<std::string_view> reset_tables{
            std::string_view{in.just_index_table_simd_per_fragment},
            std::string_view{in.just_index_table_simd_per_sequence},
            std::string_view{in.just_index_table_scalar_per_fragment},
            std::string_view{in.just_index_table_scalar_per_sequence},
            std::string_view{in.just_index_table_gpu_per_fragment},
            std::string_view{in.just_index_table_gpu_per_sequence},
            std::string_view{in.no_index_table} };
        for (const auto& [identity, table] : in.matrix_just_index_tables) {
            (void)identity;
            reset_tables.emplace_back(table);
        }
        if (!in.matrix_query_table.empty()) {
            reset_tables.emplace_back(in.matrix_query_table);
        }
        it::reset_extension(*in.conn, reset_tables);
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
    if (requires_persisted_mps_resource_preflight(setup_requirements)) {
        preflight_e2e_resources(*in.conn, in.size_label, in.target_bp);
    }

    // (4) force the fastest bioseq combo for the whole session.
    configure_bioseq_simd_session(*in.conn, in.kmer_size, in.window_size);

    const char* just_index_setup_only_env =
        std::getenv("BIOSEQ_E2E_JUST_INDEX_SETUP_ONLY");
    const std::string just_index_setup_only_name =
        just_index_setup_only_env != nullptr
            ? std::string{just_index_setup_only_env}
            : jstr(data, "just_index_setup_only", "");
    if (!just_index_setup_only_name.empty()
        && just_index_setup_only_name != k_minimizers_per_fragment_index
        && just_index_setup_only_name != k_minimizers_per_sequence_index) {
        bioseq_log<logging_level::error>("[bench_e2e] unsupported BIOSEQ_E2E_JUST_INDEX_SETUP_ONLY='{}'",
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
    in.algorithm_matrix_measurement_scope = std::ranges::all_of(
        constraints.execution_tuples,
        [](const auto& tuple) {
            return tuple.algorithm_name ==
                    bioseq::bench::e2e::detail::algorithm_matrix_no_index
                || tuple.algorithm_name ==
                    bioseq::bench::e2e::detail::algorithm_matrix_just_index
                || tuple.algorithm_name == "minimap2_no_index"
                || tuple.algorithm_name == "minimap2_just_index";
        });
    const auto requires_bioseq_no_index_table = [](std::string_view algorithm) {
        return (algorithm.starts_with("bioseq_")
                && algorithm.ends_with("_no_index"))
            || algorithm ==
                bioseq::bench::e2e::detail::sliced_no_index_algorithm
            || algorithm == "bioseq_load_only"
            || algorithm == "bioseq_recompute_only";
    };
    const bool algorithm_matrix_no_index_selected = std::ranges::any_of(
        constraints.execution_tuples,
        [](const auto& tuple) {
            return tuple.algorithm_name ==
                bioseq::bench::e2e::detail::algorithm_matrix_no_index;
        });
    const bool other_bioseq_no_index_table_selected = std::ranges::any_of(
        constraints.execution_tuples,
        [&requires_bioseq_no_index_table](const auto& tuple) {
            return requires_bioseq_no_index_table(tuple.algorithm_name)
                && tuple.algorithm_name !=
                    bioseq::bench::e2e::detail::algorithm_matrix_no_index;
        });
    const bool bioseq_no_index_table_selected =
        algorithm_matrix_no_index_selected
        || other_bioseq_no_index_table_selected;
    const e2e_setup_selection setup_selection =
        e2e_setup_selection_for(setup_requirements);
    const bool setup_only_mismatch =
        just_index_setup_only_name == k_minimizers_per_fragment_index
            ? setup_requirements.scalar_per_sequence
                || setup_requirements.simd_per_sequence
                || setup_requirements.gpu_per_sequence
            : just_index_setup_only_name == k_minimizers_per_sequence_index
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
    const auto setup_just_index_table =
        [&in, &just_index_setup_pipeline_mode](
                const std::string& table,
                std::string_view index_name,
                just_index_setup_kind setup_kind,
                const e2e_just_index_setup_requirements::matrix_setup* matrix_setup) {
            if (matrix_setup != nullptr) {
                captured_query_rows table_setup =
                    create_e2e_table_capturing_error(
                        *in.conn, table, in.seq_type);
                if (!is_exact_command_ok(table_setup)) {
                    if (!is_allowlisted_algorithm_matrix_table_setup_rejection(
                            table_setup, in)) {
                        abort_unexpected_e2e_table_setup_result(
                            table_setup, table);
                    }
                    const bool inserted =
                        in.matrix_just_index_setup_rejections.emplace(
                            matrix_setup->identity,
                            std::move(table_setup)).second;
                    if (!inserted) {
                        std::fprintf(
                            stderr,
                            "[bench_e2e] duplicate matrix table setup "
                            "rejection identity=%s\n",
                            matrix_setup->identity.c_str());
                        std::fflush(stderr);
                        std::abort();
                    }
                    return;
                }
            } else {
                create_e2e_table(*in.conn, table, in.seq_type);
            }
            configure_bioseq_scheduler_session(
                *in.conn, just_index_setup_pipeline_mode);
            if (matrix_setup != nullptr) {
                const bool vulkan_minimizer =
                    matrix_setup->minimizer_algorithm == "vulkan_minimap_integer";
                configure_bioseq_algorithm_matrix_legacy_session(
                    *in.conn, in.kmer_size, in.window_size,
                    matrix_setup->minimizer_algorithm,
                    vulkan_minimizer ? "vulkan_tiled_dynamic_programming"
                                     : "scalar_minimap2",
                    vulkan_minimizer ? "block_aligner" : "ksw2",
                    /*build_target=*/true);
            } else if (setup_kind == just_index_setup_kind::gpu) {
                // Configure Vulkan minimization and indexing before the
                // persisted build used by the untimed just-index setup.
                configure_bioseq_vulkan_index_session(
                    *in.conn,
                    in.kmer_size,
                    in.window_size);
                it::exec_sql(
                    *in.conn,
                    "DO $$ BEGIN "
                    "PERFORM current_setting('bioseq.aligner.vulkan.enabled'); "
                    "IF current_setting('bioseq.aligner.minimizer.algorithm') "
                    "   <> 'vulkan_minimap_integer' THEN "
                    "  RAISE EXCEPTION 'GPU just_index setup did not select Vulkan minimizers'; "
                    "END IF; "
                    "IF current_setting('bioseq.aligner.vulkan.index_build.enabled') "
                    "   <> 'on' THEN "
                    "  RAISE EXCEPTION 'GPU just_index setup did not enable GPU index build'; "
                    "END IF; "
                    "END $$;");
            } else if (setup_kind == just_index_setup_kind::simd) {
                configure_bioseq_simd_session(
                    *in.conn,
                    in.kmer_size,
                    in.window_size);
                it::exec_sql(
                    *in.conn,
                    "DO $$ BEGIN "
                    "IF current_setting('bioseq.aligner.minimizer.algorithm') "
                    "   <> 'two_stack_nthash' THEN "
                    "  RAISE EXCEPTION 'SIMD just_index setup did not select two_stack_nthash'; "
                    "END IF; "
                    "IF current_setting('bioseq.aligner.chaining.algorithm') "
                    "   <> 'simd_minimap2' THEN "
                    "  RAISE EXCEPTION 'SIMD just_index setup did not select simd_minimap2'; "
                    "END IF; "
                    "IF current_setting('bioseq.aligner.vulkan.enabled') "
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
                    "IF current_setting('bioseq.aligner.minimizer.algorithm') "
                    "   <> 'scalar' THEN "
                    "  RAISE EXCEPTION 'Scalar just_index setup did not select scalar minimizers'; "
                    "END IF; "
                    "IF current_setting('bioseq.aligner.chaining.algorithm') "
                    "   <> 'scalar_minimap2' THEN "
                    "  RAISE EXCEPTION 'Scalar just_index setup did not select scalar_minimap2'; "
                    "END IF; "
                    "IF current_setting('bioseq.aligner.base_alignment.algorithm') "
                    "   <> 'ksw2' THEN "
                    "  RAISE EXCEPTION 'Scalar just_index setup did not select KSW2'; "
                    "END IF; "
                    "IF current_setting('bioseq.aligner.vulkan.enabled') "
                    "   <> 'off' THEN "
                    "  RAISE EXCEPTION 'Scalar just_index setup left Vulkan enabled'; "
                    "END IF; "
                    "END $$;");
            }
            configure_bioseq_minimizer_policy(*in.conn, table, index_name);
            // The production publisher owns the PostgreSQL-backed generation;
            // there is no session-selectable payload-storage route.
            // A just_index sample owns a fully published PostgreSQL generation
            // before timing starts.  Load only the base sequence first, then
            // build the selected minimizer index explicitly: the INSERT hook
            // does not establish the READY-generation identity that the
            // post-restart reader validates.
            it::exec_sql(*in.conn, "SET bioseq.index.insert_maintenance.enabled = off;");
            it::exec_sql(*in.conn, "SET bioseq.index.async_persistence.enabled = off;");
            it::exec_sql(*in.conn, "SET bioseq.index.recompute_async_persistence.enabled = off;");
            it::exec_sql(*in.conn, "SET bioseq.index.sequence_read.enabled = on;");
            it::exec_sqlf(
                *in.conn,
                "SELECT bioseq.load('{}'::regclass, '{}');",
                table,
                in.target_fasta);
            const std::int64_t sequence_rowno =
                capture_only_sequence_rowno(*in.conn, table);
            const std::string update_sql = fmt::format(
                "SELECT bioseq.update_index('{}'::regclass, '{}');",
                table,
                index_name);
            std::int64_t indexed_sequences = 0;
            if (matrix_setup != nullptr) {
                captured_query_rows captured =
                    query_rows_capturing_error(*in.conn, update_sql);
                if (is_allowlisted_algorithm_matrix_setup_rejection(
                        captured, in, matrix_setup->minimizer_algorithm,
                        index_name)
                    || is_allowlisted_algorithm_matrix_vulkan_hardware_rejection(
                        captured, in, matrix_setup->minimizer_algorithm,
                        index_name)) {
                    const bool inserted =
                        in.matrix_just_index_setup_rejections.emplace(
                            matrix_setup->identity, std::move(captured)).second;
                    if (!inserted) {
                        std::fprintf(stderr,
                            "[bench_e2e] duplicate matrix setup rejection identity=%s\n",
                            matrix_setup->identity.c_str());
                        std::fflush(stderr);
                        std::abort();
                    }
                    return;
                }
                if (captured.status != captured_query_status::tuples_ok ||
                    captured.rows.size() != 1u ||
                    captured.rows.front().size() != 1u ||
                    !captured.rows.front().front().has_value()) {
                    std::fprintf(
                        stderr,
                        "[bench_e2e] MATRIX-SETUP-FATAL identity=%s status=%d "
                        "result_status=%s sqlstate=%s primary=%s detail=%s hint=%s "
                        "connection=%s\n",
                        matrix_setup->identity.c_str(),
                        static_cast<int>(captured.status),
                        captured.result_status.c_str(),
                        captured.sqlstate.c_str(),
                        captured.primary.c_str(),
                        captured.detail.c_str(),
                        captured.hint.c_str(),
                        captured.connection_diagnostic.c_str());
                    std::fflush(stderr);
                    std::abort();
                }
                const auto parsed = parse_i64_exact(
                    *captured.rows.front().front());
                if (!parsed.has_value()) {
                    std::fputs(
                        "[bench_e2e] MATRIX-SETUP-FATAL bioseq.update_index returned a non-integer tuple\n",
                        stderr);
                    std::fflush(stderr);
                    std::abort();
                }
                indexed_sequences = *parsed;
            } else {
                indexed_sequences = it::query_scalar_int(*in.conn, update_sql);
            }
            if (indexed_sequences <= 0) {
                bioseq_log<logging_level::error>(
                    "[bench_e2e] just_index setup published no generation "
                    "for table={} index={}",
                    table,
                    index_name);
                std::abort();
            }
            const bool gpu_seed_setup = matrix_setup != nullptr
                ? matrix_setup->minimizer_algorithm == "vulkan_minimap_integer"
                : setup_kind == just_index_setup_kind::gpu;
            if (!e2e_expects_hash_major(in, index_name, gpu_seed_setup)) {
            const std::string audit_sql = fmt::format(
                "SELECT current_setting('bioseq.aligner.minimizer.algorithm'), "
                "current_setting('bioseq.aligner.vulkan.enabled'), "
                "current_setting('bioseq.aligner.vulkan.index_build.enabled'), "
                "(current_setting('bioseq.index.insert_maintenance.enabled') = 'off')::int, "
                "(current_setting('bioseq.index.async_persistence.enabled') = 'off')::int, "
                "(current_setting("
                "'bioseq.index.recompute_async_persistence.enabled') = 'off')::int, "
                "generation_id, sequence_rowno, "
                "gpu_abi_revision AS gpu_abi_version, total_bp_size, "
                "kmer_size, window_size "
                "FROM bioseq.inspect_ready_minimizer_generation("
                "'{}'::regclass, '{}', {})",
                table,
                index_name,
                sequence_rowno);
            it::rowset_t audit_rows;
            if (matrix_setup != nullptr) {
                captured_query_rows captured =
                    query_rows_capturing_error(*in.conn, audit_sql);
                if (captured.status != captured_query_status::tuples_ok) {
                    abort_unexpected_e2e_table_setup_result(
                        captured, matrix_setup->identity);
                }
                audit_rows = std::move(captured.rows);
            } else {
                audit_rows = it::query_rows(*in.conn, audit_sql);
            }
            const bool audit_shape_valid =
                audit_rows.size() == 1 && audit_rows.front().size() == 12;
            const std::int64_t insert_maintenance_disabled = audit_shape_valid
                ? row_i64_or_minus_one(audit_rows.front(), 3)
                : -1;
            const std::int64_t async_persist_disabled = audit_shape_valid
                ? row_i64_or_minus_one(audit_rows.front(), 4)
                : -1;
            const std::int64_t recompute_persist_disabled = audit_shape_valid
                ? row_i64_or_minus_one(audit_rows.front(), 5)
                : -1;
            const std::int64_t generation_id = audit_shape_valid
                ? row_i64_or_minus_one(audit_rows.front(), 6)
                : -1;
            const std::int64_t observed_sequence_rowno = audit_shape_valid
                ? row_i64_or_minus_one(audit_rows.front(), 7)
                : -1;
            const std::int64_t gpu_abi_version = audit_shape_valid
                ? row_i64_or_minus_one(audit_rows.front(), 8)
                : -1;
            const std::int64_t persisted_bp = audit_shape_valid
                ? row_i64_or_minus_one(audit_rows.front(), 9)
                : -1;
            const std::int64_t persisted_k = audit_shape_valid
                ? row_i64_or_minus_one(audit_rows.front(), 10)
                : -1;
            const std::int64_t persisted_w = audit_shape_valid
                ? row_i64_or_minus_one(audit_rows.front(), 11)
                : -1;
            if (!audit_shape_valid
                || indexed_sequences <= 0
                || insert_maintenance_disabled != 1
                || async_persist_disabled != 1
                || recompute_persist_disabled != 1
                || generation_id <= 0
                || observed_sequence_rowno != sequence_rowno
                || gpu_abi_version != 3
                || persisted_bp != in.target_bp
                || persisted_k != in.kmer_size
                || persisted_w != in.window_size) {
                bioseq_log<logging_level::error>(
                    "[bench_e2e] invalid just_index READY-generation setup "
                    "for table={} index={} indexed_sequences={} generation={} "
                    "sequence_rowno={}/{} gpu_abi={} persisted_bp={} "
                    "expected_bp={} k={} w={} "
                    "insert_off={} async_off={} recompute_off={}",
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
        if (!in.paired_minimap2_oracle) return;

        // Bind the freshly generated minimap2 target to the exact retained
        // source bytes before either algorithm can enter a timed iteration.
        const auto rows = it::query_rows(
            *in.conn,
            fmt::format(
                "SELECT source_content_checksum, total_bp_size, kmer_size, "
                "window_size, gpu_abi_revision "
                "FROM bioseq.inspect_ready_minimizer_generation("
                "'{}'::regclass, '{}', {})",
                table,
                k_minimizers_per_fragment_index,
                sequence_rowno));
        const bool shape_valid = rows.size() == 1u
            && rows.front().size() == 5u
            && std::ranges::all_of(
                rows.front(), [](const auto& value) noexcept {
                    return value.has_value();
                });
        const auto persisted_bp = shape_valid
            ? parse_i64_exact(*rows.front()[1]) : std::nullopt;
        const auto persisted_k = shape_valid
            ? parse_i64_exact(*rows.front()[2]) : std::nullopt;
        const auto persisted_w = shape_valid
            ? parse_i64_exact(*rows.front()[3]) : std::nullopt;
        const auto gpu_abi = shape_valid
            ? parse_i64_exact(*rows.front()[4]) : std::nullopt;
        const std::string persisted_crc64 = shape_valid
            ? *rows.front()[0] : std::string{};
        if (!shape_valid || !fixed_lower_hex(persisted_crc64, 16)
            || persisted_crc64
                != in.paired_generated_target_content_crc64
            || !persisted_bp.has_value() || *persisted_bp != in.target_bp
            || !persisted_k.has_value() || *persisted_k != in.kmer_size
            || !persisted_w.has_value() || *persisted_w != in.window_size
            || !gpu_abi.has_value() || *gpu_abi != 3
            || !in.paired_persisted_target_content_crc64.empty()) {
            std::fprintf(
                stderr,
                "[bench_e2e] PAIRED-ORACLE-TARGET-INVALID table=%s "
                "generated_crc64=%s persisted_source_crc64=%s\n",
                table.c_str(),
                in.paired_generated_target_content_crc64.c_str(),
                persisted_crc64.c_str());
            std::fflush(stderr);
            std::abort();
        }
        in.paired_persisted_target_content_crc64 = persisted_crc64;
    };

    // (5) just_index tables: each indexed cell gets its own table and policy.
    // This avoids a per-sequence run accidentally succeeding through a residual
    // per-fragment index, and keeps the per-fragment cell valid too.  Reuse
    // captures only the table-AM row key here; the exact generation/source/
    // storage-profile audit still runs immediately before and after the cold
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
        for (const auto& setup : setup_requirements.matrix_setups) {
            const std::string& table =
                in.matrix_just_index_tables.at(setup.identity);
            if (!algorithm_matrix_persisted_setup_must_reject(
                    in, setup.minimizer_algorithm, setup.minimizer_index)) {
                reuse_just_index_table(table);
                continue;
            }

            // Canonical setup capability rejections must be reprobed even when
            // fixture reuse is requested. No missing/corrupt table or different
            // server failure can be retained as if a usable generation existed.
            configure_bioseq_scheduler_session(
                *in.conn, just_index_setup_pipeline_mode);
            const bool vulkan_minimizer =
                setup.minimizer_algorithm == "vulkan_minimap_integer";
            configure_bioseq_algorithm_matrix_legacy_session(
                *in.conn, in.kmer_size, in.window_size,
                setup.minimizer_algorithm,
                vulkan_minimizer ? "vulkan_tiled_dynamic_programming"
                                 : "scalar_minimap2",
                vulkan_minimizer ? "block_aligner" : "ksw2",
                /*build_target=*/true);
            configure_bioseq_minimizer_policy(
                *in.conn, table, setup.minimizer_index);
            it::exec_sql(
                *in.conn,
                "SET bioseq.index.insert_maintenance.enabled = off;");
            it::exec_sql(
                *in.conn,
                "SET bioseq.index.async_persistence.enabled = off;");
            it::exec_sql(
                *in.conn,
                "SET bioseq.index.recompute_async_persistence.enabled = off;");
            it::exec_sql(
                *in.conn,
                "SET bioseq.index.sequence_read.enabled = on;");
            captured_query_rows captured = query_rows_capturing_error(
                *in.conn,
                fmt::format(
                    "SELECT bioseq.update_index('{}'::regclass, '{}');",
                    table,
                    setup.minimizer_index));
            if (!is_allowlisted_algorithm_matrix_setup_rejection(
                    captured, in, setup.minimizer_algorithm,
                    setup.minimizer_index)
                && !is_allowlisted_algorithm_matrix_vulkan_hardware_rejection(
                    captured, in, setup.minimizer_algorithm,
                    setup.minimizer_index)) {
                std::fprintf(
                    stderr,
                    "[bench_e2e] MATRIX-SETUP-REUSE-FATAL identity=%s status=%d "
                    "result_status=%s sqlstate=%s primary=%s detail=%s hint=%s "
                    "connection=%s\n",
                    setup.identity.c_str(),
                    static_cast<int>(captured.status),
                    captured.result_status.c_str(),
                    captured.sqlstate.c_str(),
                    captured.primary.c_str(),
                    captured.detail.c_str(),
                    captured.hint.c_str(),
                    captured.connection_diagnostic.c_str());
                std::fflush(stderr);
                std::abort();
            }
            const bool inserted =
                in.matrix_just_index_setup_rejections.emplace(
                    setup.identity, std::move(captured)).second;
            if (!inserted) {
                std::fprintf(stderr,
                    "[bench_e2e] duplicate reused matrix setup rejection identity=%s\n",
                    setup.identity.c_str());
                std::fflush(stderr);
                std::abort();
            }
        }
    } else if (!skip_index_baseline_setup
            && setup_requirements.any_bioseq()) {
        if (setup_requirements.scalar_per_fragment) {
            setup_just_index_table(
                in.just_index_table_scalar_per_fragment,
                k_minimizers_per_fragment_index,
                just_index_setup_kind::scalar, nullptr);
        }
        if (setup_requirements.simd_per_fragment) {
            setup_just_index_table(
                in.just_index_table_simd_per_fragment,
                k_minimizers_per_fragment_index,
                just_index_setup_kind::simd, nullptr);
        }
        if (setup_requirements.gpu_per_fragment) {
            setup_just_index_table(
                in.just_index_table_gpu_per_fragment,
                k_minimizers_per_fragment_index,
                just_index_setup_kind::gpu, nullptr);
        }
        if (setup_requirements.scalar_per_sequence) {
            setup_just_index_table(
                in.just_index_table_scalar_per_sequence,
                k_minimizers_per_sequence_index,
                just_index_setup_kind::scalar, nullptr);
        }
        if (setup_requirements.simd_per_sequence) {
            setup_just_index_table(
                in.just_index_table_simd_per_sequence,
                k_minimizers_per_sequence_index,
                just_index_setup_kind::simd, nullptr);
        }
        if (setup_requirements.gpu_per_sequence) {
            setup_just_index_table(
                in.just_index_table_gpu_per_sequence,
                k_minimizers_per_sequence_index,
                just_index_setup_kind::gpu, nullptr);
        }
        for (const auto& setup : setup_requirements.matrix_setups) {
            setup_just_index_table(
                in.matrix_just_index_tables.at(setup.identity),
                setup.minimizer_index,
                setup.minimizer_algorithm == "vulkan_minimap_integer"
                    ? just_index_setup_kind::gpu
                    : just_index_setup_kind::simd,
                &setup);
        }
    } else {
        bioseq_log<logging_level::debug>("[bench_e2e] skipping bioseq persisted-index setup "
                "(skip_index_baseline_setup={}, scalar_just_index={}, simd_just_index={}, gpu_just_index={})",
                skip_index_baseline_setup,
                setup_selection.scalar_just_index,
                setup_selection.simd_just_index,
                setup_selection.gpu_just_index);
    }

    // (6) A Bioseq no_index tuple needs one empty table. Each rep loads base
    // data untimed, validates both minimizer sidecars absent, restarts
    // PostgreSQL, then times only transient index construction + mapping.
    // Minimap2-only cells must not instantiate a Bioseq typmod at all.
    if (!bioseq_no_index_table_selected) {
        bioseq_log<logging_level::debug>(
            "[bench_e2e] skipping Bioseq no_index table because no selected "
            "execution tuple consumes it");
    } else if (algorithm_matrix_no_index_selected
               && !other_bioseq_no_index_table_selected) {
        captured_query_rows table_setup = create_e2e_table_capturing_error(
            *in.conn, in.no_index_table, in.seq_type);
        if (!is_exact_command_ok(table_setup)) {
            if (!is_allowlisted_algorithm_matrix_table_setup_rejection(
                    table_setup, in)) {
                abort_unexpected_e2e_table_setup_result(
                    table_setup, in.no_index_table);
            }
            if (in.matrix_no_index_setup_rejection.has_value()) {
                std::fputs(
                    "[bench_e2e] duplicate matrix no_index table setup "
                    "rejection\n",
                    stderr);
                std::fflush(stderr);
                std::abort();
            }
            in.matrix_no_index_setup_rejection.emplace(
                std::move(table_setup));
        }
    } else {
        // Legacy/non-matrix table consumers have no rejected_setup contract.
        // Their setup remains strictly fatal on every server or transport error.
        create_e2e_table(*in.conn, in.no_index_table, in.seq_type);
    }

    // A non-paired matrix campaign uses one durable query relation whose
    // setup and physical no-index proof are wholly outside every repeat.  A
    // minimap2-only input and the paired/chunk diagnostic never reach this.
    create_algorithm_matrix_query_fixture(in);

    if (in.paired_minimap2_oracle
        && (!fixed_lower_hex(
                in.paired_persisted_target_content_crc64, 16)
            || in.paired_persisted_target_content_crc64
                != in.paired_generated_target_content_crc64)) {
        std::fputs(
            "[bench_e2e] PAIRED-ORACLE-TARGET-FATAL retained target identity "
            "was not sealed before minimap2 setup\n",
            stderr);
        std::fflush(stderr);
        std::abort();
    }

    // (7) minimap2 prebuilt .mmi for the just_index baseline (untimed).
    if (!skip_minimap2_baseline_setup && setup_requirements.minimap2) {
        std::error_code remove_error;
        (void)std::filesystem::remove(in.mmi_path, remove_error);
        if (remove_error) {
            throw std::runtime_error(fmt::format(
                "could not remove stale minimap2 index '{}': {}",
                in.mmi_path, remove_error.message()));
        }
        const minimap2_helper_result setup = run_minimap2_helper(
            minimap2_receipt_mode::index_setup, 3, in.kmer_size,
            in.window_size, 0u, in.minimap2_index_parts,
            in.target_fasta_split, in.mmi_path,
            in.paired_minimap2_oracle);
        if (!setup.stdout_text.empty() ||
            setup.receipt.observed_index_parts != in.minimap2_index_parts ||
            !std::filesystem::is_regular_file(in.mmi_path)) {
            throw std::runtime_error(
                "minimap2 index setup did not produce its exact validated artifact");
        }
        if (in.paired_minimap2_oracle) {
            if (setup.artifact_path.empty()
                || !fixed_lower_hex(setup.artifact_digest, 16)
                || !fixed_lower_hex(setup.receipt.checksum, 16)) {
                throw std::runtime_error(
                    "paired minimap2 setup receipt was not durably preserved");
            }
            in.minimap2_setup_receipt_checksum = setup.receipt.checksum;
            in.minimap2_setup_receipt_artifact_path = setup.artifact_path;
            in.minimap2_setup_receipt_artifact_digest =
                setup.artifact_digest;
            in.minimap2_setup_receipt_summary = setup.receipt.summary;
        }
    } else if (skip_minimap2_baseline_setup) {
        bioseq_log<logging_level::debug>("[bench_e2e] skipping minimap2 .mmi baseline setup because "
                "BIOSEQ_E2E_SKIP_MINIMAP2_BASELINE_SETUP=1");
    } else if (!setup_requirements.minimap2) {
        bioseq_log<logging_level::debug>("[bench_e2e] skipping minimap2 .mmi setup because "
                "minimap2_just_index is not selected");
    }

    bioseq_log<logging_level::debug>("[bench_e2e] input '{}' ready (truth_start={}, rev={})",
            test_case_name, in.truth_target_start, in.truth_is_reverse);
    return in;
}

[[nodiscard]] e2e_input build_e2e_input(std::string_view data_dir,
                                 std::string_view test_case_name,
                                 simdjson::dom::element data,
                                 const run_constraints& constraints) {
    e2e_input out;
    dispatch_bench_alphabet(jstr(data, "alphabet", "dna4"),
        [&out, data_dir, test_case_name, data, &constraints]
        <sequence_alphabet p_alphabet>() {
            out = e2e_input_typed<p_alphabet>(
                data_dir, test_case_name, data, constraints);
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
/** @brief PostgreSQL process identity used to validate a real restart. */
struct postmaster_identity {
    std::string start_time;
    std::int64_t backend_pid = 0;
    std::int64_t database_oid = 0;
};

/** @brief Postmaster identities captured before and after a cold restart. */
struct cold_restart_stats {
    postmaster_identity before{};
    postmaster_identity after{};
};

[[nodiscard]] bool e2e_expects_hash_major(
        const e2e_input& in,
        std::string_view minimizer_index,
        const bool gpu_seed_selected) noexcept {
    return !gpu_seed_selected
        && minimizer_index == "minimizers_per_sequence"
        && in.alphabet == "dna4"
        && ((in.kmer_size == 15 && in.window_size == 10)
            || (in.kmer_size == 20 && in.window_size == 15));
}

[[nodiscard]] postmaster_identity capture_postmaster_identity(
        const it::connection_test_context& connection) {
    const auto rows = it::query_rows(
        connection,
        "SELECT pg_postmaster_start_time()::text, pg_backend_pid()::bigint, "
        "(SELECT oid::bigint FROM pg_database "
        " WHERE datname=current_database());");
    if (rows.size() != 1 || rows.front().size() != 3
        || !rows.front()[0].has_value() || rows.front()[0]->empty()) {
        abort_pipeline_stats_capture(
            "postmaster identity has an invalid row shape");
    }
    const auto backend_pid = parse_snapshot_i64(rows.front()[1]);
    const auto database_oid = parse_snapshot_i64(rows.front()[2]);
    if (!backend_pid.has_value() || *backend_pid == 0
        || !database_oid.has_value() || *database_oid == 0) {
        abort_pipeline_stats_capture(
            "postmaster identity is missing or ill-typed");
    }
    return postmaster_identity{
        .start_time = *rows.front()[0],
        .backend_pid = *backend_pid,
        .database_oid = *database_oid,
    };
}

inline constexpr std::string_view k_measured_query_connection_failure_reason =
    "**INVALID**: measured PostgreSQL query lost its backend connection; "
    "same postmaster/database and a changed live backend were proven";

/** @brief Exact old/live session identities after a measured-query connection loss. */
struct measured_query_connection_recovery {
    postmaster_identity measured_session{};
    postmaster_identity recovered_session{};
};

/** @brief Select the already-proven cold session which owned the measured query. */
[[nodiscard]] postmaster_identity measured_query_session_identity(
        const e2e_output& out,
        const bool persisted_tuple) {
    if (persisted_tuple) {
        return postmaster_identity{
            .start_time = out.post_restart_postmaster_start_time,
            .backend_pid = out.post_restart_backend_pid,
            .database_oid = out.post_restart_database_oid,
        };
    }
    return postmaster_identity{
        .start_time = out.cold_no_index_post_restart_postmaster_start_time,
        .backend_pid = out.cold_no_index_post_restart_backend_pid,
        .database_oid = out.cold_no_index_post_restart_database_oid,
    };
}

/**
 * @brief Reconnect after one measured-query transport loss and prove continuity.
 *
 * A changed postmaster or database is infrastructure failure, not tuple
 * evidence. A live identity query on a different backend under the same
 * postmaster/database is the sole recoverable shape.
 */
[[nodiscard]] std::optional<measured_query_connection_recovery>
reconnect_and_prove_measured_query_connection_failure(
        const e2e_input& in,
        const postmaster_identity& measured_session) {
    if (measured_session.start_time.empty()
        || measured_session.backend_pid <= 0
        || measured_session.database_oid <= 0) {
        return std::nullopt;
    }

    constexpr int k_max_reconnect_attempts = 40;
    for (int attempt = 1; attempt <= k_max_reconnect_attempts; ++attempt) {
        try {
            *in.conn = it::connection_test_context{};
            const postmaster_identity recovered_session =
                capture_postmaster_identity(*in.conn);
            if (recovered_session.start_time != measured_session.start_time
                || recovered_session.database_oid != measured_session.database_oid
                || recovered_session.backend_pid <= 0
                || recovered_session.backend_pid == measured_session.backend_pid) {
                return std::nullopt;
            }
            return measured_query_connection_recovery{
                .measured_session = measured_session,
                .recovered_session = recovered_session,
            };
        } catch (const std::exception&) {
            if (attempt == k_max_reconnect_attempts) return std::nullopt;
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    }
    return std::nullopt;
}

[[nodiscard]] nlohmann::json capture_hash_major_ready_stats(
        const it::connection_test_context& connection,
        const std::string& table,
        const std::string_view minimizer_index,
        const std::int64_t sequence_rowno,
        const e2e_input& in,
        const std::string_view requested_minimizer_algorithm) {
    const auto& runtime_profile = require_e2e_minimizer_runtime_profile(
        in, requested_minimizer_algorithm);
    const it::row_t row = it::query_one(
        connection,
        fmt::format(
            "SELECT bioseq.inspect_ready_hash_major_minimizer_generation("
            "'{}'::regclass, '{}', {});",
            table,
            minimizer_index,
            sequence_rowno));
    if (row.size() != 1) {
        abort_pipeline_stats_capture(
            "hash-major READY audit has an invalid scalar shape");
    }
    const nlohmann::json audit = parse_exact_json_cell(
        row.front(), "hash-major READY audit JSON failed to parse");
    if (!json_has_exact_keys(audit, k_hash_major_ready_audit_keys)
        || required_json_text(audit, "schema")
            != "bioseq.hash_major_minimizer_generation_inspection"
        || required_json_counter(audit, "revision") != 1
        || required_json_text(audit, "layout") != "hash_major") {
        abort_pipeline_stats_capture(
            "hash-major READY audit has the wrong exact top-level schema");
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
    if (!json_has_exact_keys(source, k_hash_major_ready_source_keys)
        || !json_has_exact_keys(head, k_hash_major_ready_head_keys)
        || !json_has_exact_keys(manifest, k_hash_major_ready_manifest_keys)
        || !json_has_exact_keys(
            compatibility, k_hash_major_ready_compatibility_keys)
        || !json_has_exact_keys(metadata, k_hash_major_ready_metadata_keys)
        || streams.size() != 3
        || !head["dense_seal_present"].is_boolean()) {
        abort_pipeline_stats_capture(
            "hash-major READY audit has the wrong exact nested schema");
    }
    for (const auto& stream : streams) {
        if (!json_has_exact_keys(stream, k_hash_major_ready_stream_keys)) {
            abort_pipeline_stats_capture(
                "hash-major READY audit stream has the wrong exact schema");
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
        && counter(source, "locator_revision") > 0
        && counter(source, "num_fragments") > 0
        && counter(source, "total_bp_size") == in.target_bp
        && counter(source, "source_epoch") > 0
        && counter(source, "alphabet_code") == runtime_profile.alphabet_id
        && counter(source, "storage_unit_byte_width") == sizeof(bench_storage_t)
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
        counter(compatibility, "alphabet_code") == runtime_profile.alphabet_id
        && counter(compatibility, "kmer_size") == in.kmer_size
        && counter(compatibility, "window_size") == in.window_size
        && counter(compatibility, "hash_width_bytes")
            == static_cast<std::uint8_t>(runtime_profile.hash_width)
        && stored_posting_width
            == (target_length <= (std::int64_t{1} << 31) ? 4 : 8)
        && counter(compatibility, "position_encoding_revision") == 1
        && counter(compatibility, "hash_semantics_revision") == 1
        && counter(compatibility, "count_semantics_revision") == 1
        && counter(compatibility, "producer_backend_identity") > 0
        && counter(compatibility, "producer_backend_revision") > 0
        && counter(compatibility, "controls_per_block")
            == bioseq::indexing::k_hash_major_minimizer_controls_per_block
        && counter(compatibility, "hash_seed") == 0;
    const bool metadata_valid =
        counter(metadata, "head_rows") == 1
        && counter(metadata, "head_bytes")
            == bioseq::indexing::k_hash_major_minimizer_head_bytes
        && counter(metadata, "manifest_rows") == 1
        && counter(metadata, "manifest_bytes")
            == bioseq::indexing::k_hash_major_minimizer_manifest_bytes;

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
                        bioseq::indexing::k_hash_major_minimizer_row_payload_bytes)
                + (logical_bytes
                        % static_cast<std::int64_t>(
                            bioseq::indexing::
                                k_hash_major_minimizer_row_payload_bytes)
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
                        k_hash_major_minimizer_directory_entry_bytes)
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
        abort_pipeline_stats_capture(
            "hash-major READY audit identity/profile/stream equations disagree");
    }
    return audit;
}

void capture_pre_restart_persisted_identity(
        const it::connection_test_context& connection,
        const std::string& table,
        const std::string_view minimizer_index,
        const std::int64_t sequence_rowno,
        const e2e_input& in,
        e2e_output& out) {
    const auto rows = it::query_rows(
        connection,
        fmt::format(
            "SELECT generation_id, source_identity, "
            "gpu_abi_revision AS gpu_abi_version, "
            "sequence_rowno, source_content_checksum "
            "FROM bioseq.inspect_ready_minimizer_generation("
            "'{}'::regclass, '{}', {})",
            table,
            minimizer_index,
            sequence_rowno));
    if (rows.size() != 1 || rows.front().size() != 5
        || !rows.front()[0].has_value()
        || !rows.front()[1].has_value()
        || !rows.front()[2].has_value()
        || !rows.front()[3].has_value()
        || !rows.front()[4].has_value()) {
        abort_pipeline_stats_capture(
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
        abort_pipeline_stats_capture(
            "pre-restart persisted generation is not the production "
            "grouped-SoA layout with GPU ABI revision 3");
    }
    const std::string& persisted_target_content_crc64 = *rows.front()[4];
    if (!fixed_lower_hex(persisted_target_content_crc64, 16)) {
        abort_pipeline_stats_capture(
            "pre-restart persisted generation has a malformed authoritative "
            "source-content checksum");
    }
    if (in.paired_minimap2_oracle
        && (in.paired_generated_target_content_crc64.size() != 16u
            || persisted_target_content_crc64
                != in.paired_generated_target_content_crc64
            || persisted_target_content_crc64
                != in.paired_persisted_target_content_crc64)) {
        std::fprintf(
            stderr,
            "[bench_e2e] PAIRED-ORACLE-TARGET-INVALID generated_crc64=%s "
            "persisted_source_crc64=%s\n",
            in.paired_generated_target_content_crc64.c_str(),
            persisted_target_content_crc64.c_str());
        std::fflush(stderr);
        std::abort();
    }
    out.pre_restart_persisted_generation_id = *generation;
    out.pre_restart_persisted_gpu_abi_version = *gpu_abi_version;
    out.pre_restart_persisted_source_identity = *rows.front()[1];
    out.pre_restart_persisted_layout = "grouped_soa";
    if (in.paired_minimap2_oracle) {
        out.paired_generated_target_content_crc64 =
            in.paired_generated_target_content_crc64;
        out.paired_persisted_target_content_crc64 =
            persisted_target_content_crc64;
    }
}

void capture_pre_restart_persisted_audit(
        const it::connection_test_context& connection,
        const std::string& table,
        const std::string_view minimizer_index,
        const std::int64_t sequence_rowno,
        const e2e_input& in,
        const std::string_view requested_minimizer_algorithm,
        e2e_output& out) {
    const bool gpu_seed_selected =
        requested_minimizer_algorithm == "vulkan_minimap_integer" ||
        table == in.just_index_table_gpu_per_sequence;
    if (!e2e_expects_hash_major(in, minimizer_index, gpu_seed_selected)) {
        capture_pre_restart_persisted_identity(
            connection, table, minimizer_index, sequence_rowno, in, out);
        return;
    }
    out.pre_restart_hash_major_ready_stats = capture_hash_major_ready_stats(
        connection, table, minimizer_index, sequence_rowno, in,
        requested_minimizer_algorithm);
    const nlohmann::json& head =
        required_json_object(out.pre_restart_hash_major_ready_stats, "head");
    const nlohmann::json& source =
        required_json_object(out.pre_restart_hash_major_ready_stats, "source");
    out.pre_restart_persisted_generation_id =
        required_json_counter(head, "generation_id");
    out.pre_restart_persisted_source_identity =
        required_json_text(source, "source_identity");
    out.pre_restart_persisted_layout = "hash_major";
}

void capture_post_restart_persisted_audit(
        const it::connection_test_context& connection,
        const std::string& table,
        const std::string_view minimizer_index,
        const std::int64_t sequence_rowno,
        const e2e_input& in,
        const std::string_view requested_minimizer_algorithm,
        e2e_output& out) {
    const bool gpu_seed_selected =
        requested_minimizer_algorithm == "vulkan_minimap_integer" ||
        table == in.just_index_table_gpu_per_sequence;
    if (!e2e_expects_hash_major(in, minimizer_index, gpu_seed_selected)) {
        out.post_restart_persisted_layout = "grouped_soa";
        return;
    }
    out.post_restart_hash_major_ready_stats = capture_hash_major_ready_stats(
        connection, table, minimizer_index, sequence_rowno, in,
        requested_minimizer_algorithm);
    out.post_restart_persisted_layout = "hash_major";
    out.format6_restart_audit_accepted =
        out.pre_restart_hash_major_ready_stats
                == out.post_restart_hash_major_ready_stats
            ? 1 : 0;
    if (out.format6_restart_audit_accepted != 1) {
        abort_pipeline_stats_capture(
            "hash-major READY identity/content/profile changed across restart");
    }
}

// Return the exact before/after identity after a successful restart.  Both
// just_index and no_index use this common mechanism but retain disjoint output
// fields and acceptance gates.
[[nodiscard]] cold_restart_stats restart_postgres_for_cold_measure(
        const e2e_input& in,
        const std::string_view lifecycle_name,
        const std::string_view cold_work) {
    const postmaster_identity before = capture_postmaster_identity(*in.conn);
    const char* cmd = std::getenv("BIOSEQ_E2E_PG_RESTART_CMD");
    if (cmd == nullptr || *cmd == '\0') {
        bioseq_log<logging_level::error>(
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
    bioseq_log<logging_level::info>(
        "[bench_e2e] restarting PostgreSQL before {} measure "
        "(fresh postmaster/backend for cold {}): {}",
        lifecycle_name,
        cold_work,
        cmd);
    const int rc = std::system(cmd);
    if (rc != 0) {
        bioseq_log<logging_level::error>("[bench_e2e] PostgreSQL restart command failed (rc={}): {} "
            "— the cold {} measurement would be invalid; aborting",
            rc, cmd, lifecycle_name);
        std::abort();
    }
    // The restart terminated the shared session's backend; rebuild it. pg_ctl -w
    // returns only once the server accepts connections, but retry briefly to
    // absorb any residual race / supervisor bgworker startup.
    constexpr int k_max_reconnect_attempts = 40;
    for (int attempt = 1; ; ++attempt) {
        try {
            *in.conn = it::connection_test_context{};
            break;
        } catch (const std::exception& e) {
            if (attempt >= k_max_reconnect_attempts) {
                bioseq_log<logging_level::error>("[bench_e2e] failed to reconnect after PostgreSQL "
                    "restart ({} attempts): {} — aborting", attempt, e.what());
                std::abort();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    }
    // The branch-specific path reapplies its full scheduler/index/backend GUC
    // set; keep the fresh session on the non-Vulkan default until then.
    configure_bioseq_simd_session(*in.conn, in.kmer_size, in.window_size);
    const postmaster_identity after = capture_postmaster_identity(*in.conn);
    if (before.start_time == after.start_time
        || before.backend_pid == after.backend_pid
        || before.database_oid != after.database_oid) {
        bioseq_log<logging_level::error>(
            "[bench_e2e] restart command returned without validating a new "
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
    bioseq_log<logging_level::info>("[bench_e2e] PostgreSQL restarted + session reconnected; the "
        "{} measurement will execute {} from a fresh backend "
        "(postmaster {} -> {}, backend {} -> {})",
        lifecycle_name,
        cold_work,
        before.start_time,
        after.start_time,
        before.backend_pid,
        after.backend_pid);
    return cold_restart_stats{.before = before, .after = after};
}

// Returns true after a restart has completed and its identity change has been
// validated. A successful restart command alone is not cold-run stats.
bool restart_postgres_for_cold_just_index(
        const e2e_input& in,
        e2e_output& out) {
    const cold_restart_stats stats = restart_postgres_for_cold_measure(
        in, "just_index", "persisted-index read plus mapping");
    out.pre_restart_postmaster_start_time = stats.before.start_time;
    out.pre_restart_backend_pid = stats.before.backend_pid;
    out.pre_restart_database_oid = stats.before.database_oid;
    out.post_restart_postmaster_start_time = stats.after.start_time;
    out.post_restart_backend_pid = stats.after.backend_pid;
    out.post_restart_database_oid = stats.after.database_oid;
    if (in.require_nice_attestation) {
        out.postgres_backend_nice = establish_e2e_process_nice(
            stats.after.backend_pid, "postgres_backend");
    }
    out.cold_restart_identity_changed = 1;
    return true;
}

bool restart_postgres_for_cold_no_index(
        const e2e_input& in,
        e2e_output& out) {
    const cold_restart_stats stats = restart_postgres_for_cold_measure(
        in, "no_index", "transient exact target-index construction plus mapping");
    out.cold_no_index_pre_restart_postmaster_start_time =
        stats.before.start_time;
    out.cold_no_index_pre_restart_backend_pid = stats.before.backend_pid;
    out.cold_no_index_pre_restart_database_oid = stats.before.database_oid;
    out.cold_no_index_post_restart_postmaster_start_time = stats.after.start_time;
    out.cold_no_index_post_restart_backend_pid = stats.after.backend_pid;
    out.cold_no_index_post_restart_database_oid = stats.after.database_oid;
    if (in.require_nice_attestation) {
        out.postgres_backend_nice = establish_e2e_process_nice(
            stats.after.backend_pid, "postgres_backend");
    }
    out.cold_no_index_restart_identity_changed = 1;
    return true;
}

void configure_no_index_storage_session(
        const it::connection_test_context& connection) {
    it::exec_sql(connection, "SET bioseq.index.insert_maintenance.enabled = off;");
    it::exec_sql(connection, "SET bioseq.index.sequence_read.enabled = off;");
    it::exec_sql(connection, "SET bioseq.index.async_persistence.enabled = off;");
    it::exec_sql(connection, "SET bioseq.index.recompute_async_persistence.enabled = off;");
}

void prepare_and_restart_cold_no_index_base(
        const e2e_input& in,
        e2e_output& out,
        const std::string_view minimizer_index,
        const std::string_view pipeline_mode) {
    // Base PostgreSQL data is setup, not target-index construction.  Load it
    // with both minimizer policies disabled, validate no retired index row
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
        *in.conn, in, out, cold_no_index_audit_stage::pre_restart);
    out.cold_no_index_restart_performed =
        restart_postgres_for_cold_no_index(in, out) ? 1 : 0;
}

void load_e2e_query_table_range(
        const it::connection_test_context& c,
        const e2e_input& in,
        const std::size_t query_begin,
        const std::size_t query_end) {
    if (query_begin >= query_end || query_end > in.queries.size()) {
        throw std::invalid_argument(
            "E2E query-table chunk is outside the canonical query set");
    }
    it::exec_sql(c, "DROP TABLE IF EXISTS pg_temp.e2e_queries;");
    it::exec_sql(c, fmt::format(
        "CREATE TEMP TABLE e2e_queries(id int PRIMARY KEY, seq {});",
        in.seq_type));
    for (std::size_t index = query_begin; index < query_end; ++index) {
        const e2e_query_truth& query = in.queries[index];
        it::exec_sql(c, fmt::format(
            "INSERT INTO e2e_queries VALUES ({}, '{}'::{});",
            query.id, query.bases, in.seq_type));
    }
}

void load_e2e_query_table(const it::connection_test_context& c,
                          const e2e_input& in) {
    load_e2e_query_table_range(c, in, 0u, in.queries.size());
}

/** Config-gated statement timeout with exact session-state restoration. */
class scoped_e2e_statement_timeout final {
public:
    scoped_e2e_statement_timeout(
            const it::connection_test_context& connection,
            const std::uint64_t timeout_ms)
        : connection_(&connection) {
        if (timeout_ms == 0 || timeout_ms > k_e2e_max_statement_timeout_ms) {
            throw std::invalid_argument(
                "E2E statement timeout is outside the cleanup-safe bound");
        }
        previous_timeout_ms_ = it::query_scalar_int(
            connection,
            "SELECT setting::bigint FROM pg_settings "
            "WHERE name='statement_timeout';");
        if (previous_timeout_ms_ < 0) {
            throw std::runtime_error(
                "PostgreSQL statement_timeout has an invalid prior value");
        }
        it::exec_sqlf(
            connection, "SET statement_timeout = {};", timeout_ms);
        active_ = true;
    }

    scoped_e2e_statement_timeout(const scoped_e2e_statement_timeout&) = delete;
    scoped_e2e_statement_timeout& operator=(
        const scoped_e2e_statement_timeout&) = delete;

    [[nodiscard]] bool restore() noexcept {
        if (!active_) return restored_;
        try {
            it::exec_sqlf(
                *connection_, "SET statement_timeout = {};",
                previous_timeout_ms_);
            active_ = false;
            restored_ = true;
        } catch (...) {
            restored_ = false;
        }
        return restored_;
    }

    ~scoped_e2e_statement_timeout() noexcept {
        (void)restore();
    }

private:
    const it::connection_test_context* connection_ = nullptr;
    std::int64_t previous_timeout_ms_ = 0;
    bool active_ = false;
    bool restored_ = false;
};

[[nodiscard]] std::string batched_coord_sql(
        const std::string& target_table,
        std::string_view projection,
        const int index_read_requirement,
        const std::string_view query_table = "e2e_queries")
{
    return fmt::format(
        "SELECT q.id, t.header, {} "
        "FROM {} t JOIN {} q ON mmsearch(t.seq, q.seq, "
        "aligner_options(index_read_requirement => {})) "
        "ORDER BY q.id, t.header",
        projection, target_table, query_table, index_read_requirement);
}

[[nodiscard]] std::string batched_transformed_coord_sql(
        const std::string& target_table,
        std::string_view projection,
        const int index_read_requirement,
        const std::int64_t target_coordinate_origin)
{
    const std::int64_t concat_join = target_coordinate_origin + 16;
    // The benchmark connection intentionally keeps the default search_path.
    // Sequence operators live in the extension-owned bioseq schema, so use
    // PostgreSQL's schema-qualified operator syntax just like production SQL
    // that does not opt in to `bioseq` on its search path.
    return fmt::format(
        "SELECT q.id, t.header, {} "
        "FROM {} t JOIN e2e_queries q ON mmsearch("
        "t.seq[{}:{}] OPERATOR(bioseq.||) t.seq[{}:], "
        "q.seq[0:], aligner_options(index_read_requirement => {})) "
        "ORDER BY q.id, t.header",
        projection, target_table, target_coordinate_origin, concat_join,
        concat_join, index_read_requirement);
}

[[nodiscard]] std::int64_t sliced_transformed_target_origin(
        const e2e_input& in) {
    if (in.queries.empty()) return 0;
    std::int64_t minimum_truth = in.queries.front().truth_target_start;
    for (const e2e_query_truth& query : in.queries) {
        minimum_truth = std::min(minimum_truth, query.truth_target_start);
    }
    if (minimum_truth <= 0) {
        std::fputs(
            "[bench_e2e] nonzero target slice requires every "
            "generated truth_target_start to be positive\n",
            stderr);
        std::fflush(stderr);
        std::abort();
    }
    return std::min<std::int64_t>(4096, minimum_truth);
}

[[nodiscard]] std::string e2e_projection_sql(const e2e_input& in) {
    if (!in.cigar_projection) {
        std::string projection = "bioseq.target_start(header) AS tstart, "
                                 "bioseq.is_reverse(header) AS rev";
        if (in.mapq_projection) projection += ", bioseq.mapq(header) AS mapq";
        return projection;
    }
    std::string projection =
        "bioseq.target_start(header) AS tstart, "
        "bioseq.target_end(header) AS tend, "
        "bioseq.query_start(header) AS qstart, "
        "bioseq.query_end(header) AS qend, "
        "bioseq.is_reverse(header) AS rev, "
        "bioseq.has_cigar(header) AS has_cigar, "
        "bioseq.cigar(header) AS cigar";
    if (in.mapq_projection) projection += ", bioseq.mapq(header) AS mapq";
    return projection;
}

[[nodiscard]] constexpr std::string_view captured_query_status_name(
        const captured_query_status status) noexcept {
    switch (status) {
    case captured_query_status::tuples_ok:
        return "tuples_ok";
    case captured_query_status::command_ok:
        return "command_ok";
    case captured_query_status::server_error:
        return "server_error";
    case captured_query_status::connection_failure:
        return "connection_failure";
    case captured_query_status::unexpected_status:
        return "unexpected_status";
    }
    return "unexpected_status";
}

[[nodiscard]] bool is_resolved_algorithm_matrix_tuple(
        const e2e_output& out) noexcept {
    const bool minimizer_resolved =
        out.requested_minimizer_algorithm == "scalar" ||
        out.requested_minimizer_algorithm == "simd_minimap2_reference" ||
        out.requested_minimizer_algorithm == "two_stack_nthash" ||
        out.requested_minimizer_algorithm == "vulkan_minimap_integer" ||
        out.requested_minimizer_algorithm == "bounded_nthash_simd";
    const bool chaining_resolved =
        out.requested_chaining_algorithm == "scalar_minimap2" ||
        out.requested_chaining_algorithm == "simd_minimap2" ||
        out.requested_chaining_algorithm == "range_minimum_query" ||
        out.requested_chaining_algorithm ==
            "vulkan_tiled_dynamic_programming";
    const bool alignment_resolved =
        out.requested_base_alignment_algorithm == "ksw2" ||
        out.requested_base_alignment_algorithm == "block_aligner";
    return minimizer_resolved && chaining_resolved && alignment_resolved;
}

[[nodiscard]] bool is_complete_vulkan_algorithm_matrix_tuple(
        const e2e_input& input,
        const e2e_output& out) noexcept {
    // CIGAR and MAPQ projection select physically attested stages within this
    // tuple; neither projection makes the requested all-Vulkan tuple partial.
    static_cast<void>(input);
    return out.requested_minimizer_algorithm == "vulkan_minimap_integer" &&
        out.requested_chaining_algorithm ==
            "vulkan_tiled_dynamic_programming" &&
        out.requested_base_alignment_algorithm == "block_aligner";
}

[[nodiscard]] bool is_allowlisted_algorithm_matrix_sql_rejection(
        const captured_query_rows& captured,
        const e2e_input& in,
        const e2e_output& out,
        const bool persisted_tuple) noexcept {
    if (captured.status != captured_query_status::server_error ||
        captured.result_status != "PGRES_FATAL_ERROR" ||
        captured.sqlstate.empty() || captured.primary.empty() ||
        !is_resolved_algorithm_matrix_tuple(out)) {
        return false;
    }
    if (captured.sqlstate == "0A000" &&
        captured.primary ==
            "Exact SIMD minimizer execution is unsupported for this request" &&
        captured.detail ==
            "capability=exact_simd_minimizer; fallback=forbidden") {
        return out.requested_minimizer_algorithm == "two_stack_nthash";
    }
    if (captured.sqlstate == "0A000" &&
        captured.primary ==
            "Bounded ntHash SIMD minimizer execution is unsupported for this request" &&
        captured.detail ==
            "capability=bounded_nthash_simd_minimizer; fallback=forbidden") {
        return out.requested_minimizer_algorithm == "bounded_nthash_simd";
    }
    if (captured.sqlstate == "0A000" &&
        captured.primary ==
            "SIMD chaining execution is unsupported for this request" &&
        captured.detail == "capability=simd_chaining; fallback=forbidden") {
        return out.requested_chaining_algorithm == "simd_minimap2";
    }
    const bool requests_vulkan_component =
        out.requested_minimizer_algorithm == "vulkan_minimap_integer" ||
        out.requested_chaining_algorithm ==
            "vulkan_tiled_dynamic_programming";
    const bool complete_vulkan_tuple =
        is_complete_vulkan_algorithm_matrix_tuple(in, out);
    if (captured.sqlstate == "22023" &&
        captured.primary ==
            "required persisted GPU grouped-SoA target is unavailable" &&
        captured.detail ==
            "GPU prepared-target identity, pinned k/w, or all-GPU selector is invalid") {
        return persisted_tuple && requests_vulkan_component;
    }
    if (captured.sqlstate == "0A000" &&
        captured.primary ==
            "persisted GPU grouped-SoA execution does not yet support CIGAR projection" &&
        captured.detail ==
            "the direct path can execute query sketching, persisted Shape lookup, GPU chaining, "
            "backtracking, and coordinate projection, but no genuine GPU target-window reader "
            "is wired for alignment extension") {
        return persisted_tuple && complete_vulkan_tuple;
    }
    if (captured.sqlstate == "0A000" &&
        captured.primary == "Vulkan algorithm tuple is unsupported" &&
        captured.detail ==
            "requested Vulkan components do not form the complete supported all-GPU tuple; "
            "generic CPU/SIMD biological fallback is forbidden") {
        return requests_vulkan_component && !complete_vulkan_tuple;
    }
    if (captured.sqlstate == "0A000" &&
        captured.primary ==
            "Vulkan simple-CIGAR span exceeds the supported ABI limit" &&
        captured.detail ==
            "simple_cigar_max_span=16384; one or more selected alignment spans exceeded this limit") {
        return !persisted_tuple && complete_vulkan_tuple;
    }
    return false;
}

[[noreturn]] void abort_unexpected_algorithm_matrix_query_result(
        const captured_query_rows& captured,
        const e2e_output& out) {
    std::fprintf(
        stderr,
        "[bench_e2e] MATRIX-SQL-FATAL status=%.*s result_status=%s "
        "sqlstate=%s primary=%s detail=%s hint=%s connection=%s "
        "minimizer=%s chaining=%s alignment=%s\n",
        static_cast<int>(captured_query_status_name(captured.status).size()),
        captured_query_status_name(captured.status).data(),
        captured.result_status.c_str(),
        captured.sqlstate.c_str(),
        captured.primary.c_str(),
        captured.detail.c_str(),
        captured.hint.c_str(),
        captured.connection_diagnostic.c_str(),
        out.requested_minimizer_algorithm.c_str(),
        out.requested_chaining_algorithm.c_str(),
        out.requested_base_alignment_algorithm.c_str());
    std::fflush(stderr);
    std::abort();
}

/** @brief Serialize one proven measured-query backend replacement as invalid evidence. */
void record_measured_query_connection_failure(
        const captured_query_rows& captured,
        const e2e_input& in,
        const measured_query_connection_recovery& recovery,
        e2e_output& out) {
    const bool exact_transport_diagnostic =
        (captured.result_status == "PGRES_FATAL_ERROR"
            || captured.result_status == "null_result")
        && captured.sqlstate.empty()
        && captured.primary.empty()
        && captured.detail.empty();
    if (captured.status != captured_query_status::connection_failure
        || !exact_transport_diagnostic) {
        abort_unexpected_algorithm_matrix_query_result(captured, out);
    }
    const bool same_postmaster =
        recovery.measured_session.start_time == recovery.recovered_session.start_time;
    const bool same_database =
        recovery.measured_session.database_oid == recovery.recovered_session.database_oid;
    const bool changed_live_backend =
        recovery.measured_session.backend_pid > 0
        && recovery.recovered_session.backend_pid > 0
        && recovery.measured_session.backend_pid
            != recovery.recovered_session.backend_pid;
    if (!same_postmaster || !same_database || !changed_live_backend) {
        abort_unexpected_algorithm_matrix_query_result(captured, out);
    }

    out.tuple_accepted = 0;
    out.cell_disposition = "invalid";
    out.phase = "measured_statement";
    out.correctness_evaluated = 0;
    out.expected_match_count = static_cast<std::int64_t>(in.queries.size());
    out.sql_result_status = captured.result_status;
    out.sqlstate = captured.sqlstate;
    out.sql_primary = captured.primary;
    out.sql_detail = captured.detail;
    out.sql_hint = captured.hint;
    out.sql_connection_diagnostic = captured.connection_diagnostic;
    out.tuple_rejection_reason = k_measured_query_connection_failure_reason;
    out.measured_query_connection_failure_proof = nlohmann::json{
        {"schema", "bioseq.e2e.measured-query-connection-failure"},
        {"version", 1},
        {"captured_status", "connection_failure"},
        {"same_postmaster", same_postmaster},
        {"same_database", same_database},
        {"changed_live_backend", changed_live_backend},
        {"live_identity_query_succeeded", true},
        {"measured_session", nlohmann::json{
            {"postmaster_start", recovery.measured_session.start_time},
            {"backend_pid", recovery.measured_session.backend_pid},
            {"database_oid", recovery.measured_session.database_oid},
        }},
        {"recovered_session", nlohmann::json{
            {"postmaster_start", recovery.recovered_session.start_time},
            {"backend_pid", recovery.recovered_session.backend_pid},
            {"database_oid", recovery.recovered_session.database_oid},
        }},
    };
}

void record_allowlisted_algorithm_matrix_sql_rejection(
        const captured_query_rows& captured,
        const e2e_input& in,
        e2e_output& out) {
    out.tuple_accepted = 0;
    out.cell_disposition = "rejected_sql";
    out.phase = "measured_statement";
    out.correctness_evaluated = 0;
    out.expected_match_count =
        static_cast<std::int64_t>(in.queries.size());
    out.sql_result_status = captured.result_status;
    out.sqlstate = captured.sqlstate;
    out.sql_primary = captured.primary;
    out.sql_detail = captured.detail;
    out.sql_hint = captured.hint;
    out.sql_connection_diagnostic = captured.connection_diagnostic;
    out.tuple_rejection_reason = fmt::format(
        "allowlisted PostgreSQL tuple rejection SQLSTATE={} primary={}",
        captured.sqlstate,
        captured.primary);
}

void append_behavior_difference(
        e2e_output& out,
        std::string_view comment) {
    if (!out.behavior_difference_comment.empty()) {
        out.behavior_difference_comment += "; ";
    }
    out.behavior_difference_comment.append(comment.data(), comment.size());
}

void annotate_accepted_behavior_differences(
        const e2e_input& in,
        e2e_output& out) {
    if (out.tuple_accepted != 1) return;
    if (out.requested_minimizer_algorithm == "simd_minimap2_reference" &&
        out.observed_minimizer_algorithm == "scalar") {
        out.cell_disposition = "accepted_fallback";
        append_behavior_difference(
            out,
            "simd_minimap2_reference completed through the operation-observed "
            "scalar minimizer route");
    }
    // GPU seed/score selection does not imply GPU CIGAR or GPU MAPQ.
    const bool vulkan_route = out.actual_backend == "vulkan_gpu";
    if (vulkan_route && out.requested_base_alignment_algorithm == "block_aligner" &&
        out.observed_base_alignment_algorithm == "vulkan_simple_cigar") {
        if (out.cell_disposition == "accepted") {
            out.cell_disposition = "accepted_behavior_difference";
        }
        append_behavior_difference(
            out,
            "requested block_aligner was observed as vulkan_simple_cigar; "
            "vulkan_simple_cigar is the distinct Vulkan-native CIGAR "
            "implementation selected through the public base-alignment slot, "
            "so this is an accepted naming/behavior difference rather than "
            "a fallback; "
            "host copied native packed storage units without rank decoding, "
            "while the device performed orientation, alignment, traceback, "
            "CIGAR, and match counting; no CPU/SIMD biological fallback ran");
    }
    if (in.mapq_projection && vulkan_route && out.mapq_execution_available == 1
        && out.mapq_executed == 1) {
        append_behavior_difference(
            out,
            "Vulkan MAPQ was physically executed and range-validated; its "
            "numeric vector is diagnostic and does not claim minimap2 MAPQ "
            "semantics");
    }
}

void finalize_e2e_priority_attestation(
        const std::string_view algorithm,
        const e2e_input& in,
        e2e_output& out) {
    if (!in.require_nice_attestation) return;
    const bool postgres_algorithm = algorithm.starts_with("bioseq_");
    out.priority_attestation_accepted =
        out.priority_target_nice == k_normal_user_best_nice
        && out.runner_pid == in.runner_pid
        && out.runner_nice == k_normal_user_best_nice
        && (!postgres_algorithm
            || out.postgres_backend_nice == k_normal_user_best_nice)
            ? 1 : 0;
    if (out.tuple_accepted == 1
        && out.priority_attestation_accepted != 1) {
        std::fprintf(
            stderr,
            "[bench_e2e] PRIORITY-ATTESTATION-FATAL algorithm=%.*s "
            "target=%lld runner_pid=%lld runner_nice=%lld backend_nice=%lld\n",
            static_cast<int>(algorithm.size()), algorithm.data(),
            static_cast<long long>(out.priority_target_nice),
            static_cast<long long>(out.runner_pid),
            static_cast<long long>(out.runner_nice),
            static_cast<long long>(out.postgres_backend_nice));
        std::fflush(stderr);
        std::abort();
    }
}

[[nodiscard]] nlohmann::json serialize_alignment_snapshot_rows(
    const e2e_output& out,
    bool include_numeric_timings);

[[nodiscard]] std::int64_t checked_logical_i64_sum(
        const std::int64_t lhs,
        const std::int64_t rhs,
        const std::string_view label) {
    const __int128 sum = static_cast<__int128>(lhs)
        + static_cast<__int128>(rhs);
    if (lhs < 0 || rhs < 0 || sum < 0
        || sum > (std::numeric_limits<std::int64_t>::max)()) {
        throw std::runtime_error(fmt::format(
            "logical E2E {} counter overflow or negative input", label));
    }
    return static_cast<std::int64_t>(sum);
}

[[nodiscard]] std::uint64_t checked_logical_u64_sum(
        const std::uint64_t lhs,
        const std::uint64_t rhs,
        const std::string_view label) {
    if (rhs > (std::numeric_limits<std::uint64_t>::max)() - lhs) {
        throw std::runtime_error(fmt::format(
            "logical E2E {} duration overflow", label));
    }
    return lhs + rhs;
}

void record_logical_chunk_failure(
        e2e_output& out,
        const std::string_view algorithm,
        const logical_query_chunk chunk,
        const std::string_view detail) {
    out.tuple_accepted = 0;
    out.cell_disposition = "invalid";
    out.phase = "logical_query_chunk";
    out.correctness_evaluated = 0;
    out.tuple_rejection_reason = fmt::format(
        "**INVALID**: logical paired row algorithm={} chunk=[{},{}) failed: {}",
        algorithm, chunk.begin, chunk.end, detail);
}

[[nodiscard]] nlohmann::json make_postgres_logical_chunk_receipt(
        const e2e_output& chunk_output,
        const logical_query_chunk chunk,
        const std::size_t ordinal,
        const std::uint64_t timeout_ms,
        const bool timeout_restored) {
    return nlohmann::json{
        {"ordinal", ordinal},
        {"query_begin", chunk.begin},
        {"query_end", chunk.end},
        {"query_count", chunk.size()},
        {"statement_timeout_ms", timeout_ms},
        {"statement_timeout_restored", timeout_restored},
        {"postmaster_start_time", chunk_output.timed_postmaster_start_time},
        {"postgres_backend_pid", chunk_output.alignment_backend_pid},
        {"database_oid", chunk_output.alignment_database_oid},
        {"persisted_generation_id", chunk_output.persisted_generation_id},
        {"measured_algorithm_ns", chunk_output.measured_region_ns},
        {"tuple_accepted", chunk_output.tuple_accepted},
        {"pipeline_validation_accepted",
            chunk_output.pipeline_stats_consistent == 1},
        {"stage_engine_matrix_valid",
            chunk_output.stage_engine_matrix_valid == 1},
        {"backend", nlohmann::json{
            {"requested", chunk_output.requested_backend},
            {"actual", chunk_output.actual_backend},
            {"declines", chunk_output.backend_declines},
            {"fallbacks", chunk_output.backend_fallbacks},
            {"cpu_or_simd_biological_fallbacks",
                chunk_output.block_aligner_forbidden_scalar_biological_fallbacks},
        }},
        {"cardinality", nlohmann::json{
            {"attempted_jobs", chunk_output.attempted_alignment_jobs},
            {"fully_attested_jobs",
                chunk_output.fully_attested_alignment_jobs},
            {"unattested_jobs", chunk_output.unattested_alignment_jobs},
        }},
        {"dispatch", nlohmann::json{
            {"seed", chunk_output.gpu_seed_dispatches},
            {"chain", chunk_output.gpu_chain_dispatches},
            {"backtrack", chunk_output.gpu_chain_backtrack_dispatches},
            {"extend", chunk_output.gpu_extend_dispatches},
            {"resident_uploads",
                chunk_output.gpu_persisted_resident_uploads},
            {"resident_cache_hits",
                chunk_output.gpu_persisted_resident_cache_hits},
        }},
        {"interval_receipt", nlohmann::json{
            {"schema_revision",
                chunk_output.benchmark_e2e_interval_schema_revision},
            {"available", chunk_output.benchmark_e2e_interval_available},
            {"union_ns", chunk_output.benchmark_e2e_interval_union_ns},
            {"interval_count", chunk_output.benchmark_e2e_interval_count},
            {"cigar_interval_count",
                chunk_output.benchmark_e2e_cigar_interval_count},
            {"mapq_interval_count",
                chunk_output.benchmark_e2e_mapq_interval_count},
            {"finalization_count",
                chunk_output.benchmark_e2e_interval_finalization_count},
            {"failure_code",
                chunk_output.benchmark_e2e_interval_failure_code},
            {"origins", chunk_output.benchmark_e2e_interval_origins},
        }},
        {"alignment_snapshot",
            serialize_alignment_snapshot_rows(chunk_output, true)},
        {"persisted_gpu_operation_stats",
            chunk_output.persisted_gpu_operation_stats},
        {"minimizer_fragment_reader_stats",
            chunk_output.minimizer_fragment_reader_stats},
        {"hash_major_index_reader_stats",
            chunk_output.hash_major_index_reader_stats},
        {"validation_excluded_ns", chunk_output.validation_excluded_ns},
    };
}

[[nodiscard]] bool logical_postgres_chunk_identity_matches(
        const e2e_output& aggregate,
        const e2e_output& chunk) noexcept {
    return aggregate.requested_backend == chunk.requested_backend
        && aggregate.actual_backend == chunk.actual_backend
        && aggregate.requested_target_index_source
            == chunk.requested_target_index_source
        && aggregate.actual_target_index_source
            == chunk.actual_target_index_source
        && aggregate.attested_requested_minimizer_algorithm
            == chunk.attested_requested_minimizer_algorithm
        && aggregate.observed_minimizer_algorithm
            == chunk.observed_minimizer_algorithm
        && aggregate.attested_requested_chaining_algorithm
            == chunk.attested_requested_chaining_algorithm
        && aggregate.observed_chaining_algorithm
            == chunk.observed_chaining_algorithm
        && aggregate.attested_requested_base_alignment_algorithm
            == chunk.attested_requested_base_alignment_algorithm
        && aggregate.observed_base_alignment_algorithm
            == chunk.observed_base_alignment_algorithm
        && aggregate.persisted_generation_id == chunk.persisted_generation_id
        && aggregate.persisted_source_identity
            == chunk.persisted_source_identity
        && aggregate.gpu_device_uuid == chunk.gpu_device_uuid
        && aggregate.gpu_driver == chunk.gpu_driver
        && aggregate.gpu_pipeline_cache_seed_sha256
            == chunk.gpu_pipeline_cache_seed_sha256
        && aggregate.gpu_pipeline_cache_manifest_checksum
            == chunk.gpu_pipeline_cache_manifest_checksum
        && aggregate.mapq_execution_available
            == chunk.mapq_execution_available
        && aggregate.mapq_executed == chunk.mapq_executed
        && aggregate.mapq_execution_conflict
            == chunk.mapq_execution_conflict
        && aggregate.postgres_backend_nice == chunk.postgres_backend_nice
        && aggregate.timed_postmaster_start_time
            == chunk.timed_postmaster_start_time
        && aggregate.post_restart_postmaster_start_time
            == chunk.post_restart_postmaster_start_time
        && aggregate.post_restart_backend_pid
            == chunk.post_restart_backend_pid
        && aggregate.post_restart_database_oid
            == chunk.post_restart_database_oid
        && aggregate.alignment_backend_pid == chunk.alignment_backend_pid
        && aggregate.alignment_database_oid == chunk.alignment_database_oid;
}

void append_shifted_interval_origins(
        nlohmann::json& destination,
        const nlohmann::json& origins,
        const std::size_t query_begin) {
    if (!origins.is_array()) {
        throw std::runtime_error(
            "logical E2E interval origins are not an array");
    }
    if (!destination.is_array()) destination = nlohmann::json::array();
    for (nlohmann::json origin : origins) {
        if (!origin.is_object() || !origin.contains("query_ordinal")
            || !origin["query_ordinal"].is_number_integer()) {
            throw std::runtime_error(
                "logical E2E interval origin has no exact query ordinal");
        }
        const std::int64_t local = origin["query_ordinal"].get<std::int64_t>();
        const auto maximum_ordinal = static_cast<std::uint64_t>(
            (std::numeric_limits<std::int64_t>::max)());
        if (local < 0
            || query_begin > maximum_ordinal
            || static_cast<std::uint64_t>(local)
                > maximum_ordinal - query_begin) {
            throw std::runtime_error(
                "logical E2E interval query ordinal overflow");
        }
        origin["query_ordinal"] = static_cast<std::int64_t>(
            query_begin + static_cast<std::size_t>(local));
        destination.push_back(std::move(origin));
    }
}

void merge_logical_postgres_chunk(
        e2e_output& aggregate,
        e2e_output chunk,
        const logical_query_chunk range,
        const bool first) {
    if (first) {
        aggregate = std::move(chunk);
        nlohmann::json shifted = nlohmann::json::array();
        append_shifted_interval_origins(
            shifted, aggregate.benchmark_e2e_interval_origins, range.begin);
        aggregate.benchmark_e2e_interval_origins = std::move(shifted);
        return;
    }
    if (!logical_postgres_chunk_identity_matches(aggregate, chunk)) {
        throw std::runtime_error(
            "logical E2E PostgreSQL chunk changed backend/generation identity");
    }
    const auto add = [](std::int64_t& destination,
                        const std::int64_t value,
                        const std::string_view label) {
        destination = checked_logical_i64_sum(destination, value, label);
    };
    add(aggregate.attempted_alignment_jobs,
        chunk.attempted_alignment_jobs, "attempted jobs");
    add(aggregate.fully_attested_alignment_jobs,
        chunk.fully_attested_alignment_jobs, "attested jobs");
    add(aggregate.unattested_alignment_jobs,
        chunk.unattested_alignment_jobs, "unattested jobs");
    add(aggregate.gpu_persisted_resident_uploads,
        chunk.gpu_persisted_resident_uploads, "resident uploads");
    add(aggregate.gpu_persisted_resident_cache_hits,
        chunk.gpu_persisted_resident_cache_hits, "resident cache hits");
    add(aggregate.gpu_seed_dispatches,
        chunk.gpu_seed_dispatches, "seed dispatches");
    add(aggregate.gpu_chain_dispatches,
        chunk.gpu_chain_dispatches, "chain dispatches");
    add(aggregate.gpu_chain_backtrack_dispatches,
        chunk.gpu_chain_backtrack_dispatches, "backtrack dispatches");
    add(aggregate.gpu_extend_dispatches,
        chunk.gpu_extend_dispatches, "extend dispatches");
    add(aggregate.backend_declines,
        chunk.backend_declines, "backend declines");
    add(aggregate.backend_fallbacks,
        chunk.backend_fallbacks, "backend fallbacks");
    add(aggregate.host_query_hash_readbacks,
        chunk.host_query_hash_readbacks, "host query readbacks");
    add(aggregate.host_query_symbol_packing_calls,
        chunk.host_query_symbol_packing_calls, "host query packing");
    add(aggregate.host_target_symbol_packing_calls,
        chunk.host_target_symbol_packing_calls, "host target packing");
    add(aggregate.host_alignment_job_classification_calls,
        chunk.host_alignment_job_classification_calls,
        "host job classification");
    add(aggregate.host_cigar_construction_calls,
        chunk.host_cigar_construction_calls, "host CIGAR construction");
    add(aggregate.prefilter_activity_count,
        chunk.prefilter_activity_count, "prefilter activity");
    add(aggregate.benchmark_e2e_interval_union_ns,
        chunk.benchmark_e2e_interval_union_ns, "interval union");
    add(aggregate.benchmark_e2e_interval_count,
        chunk.benchmark_e2e_interval_count, "interval count");
    add(aggregate.benchmark_e2e_cigar_interval_count,
        chunk.benchmark_e2e_cigar_interval_count,
        "CIGAR interval count");
    add(aggregate.benchmark_e2e_mapq_interval_count,
        chunk.benchmark_e2e_mapq_interval_count,
        "MAPQ interval count");
    add(aggregate.benchmark_e2e_interval_finalization_count,
        chunk.benchmark_e2e_interval_finalization_count,
        "interval finalization count");
    add(aggregate.persisted_gpu_dropped_operation_count,
        chunk.persisted_gpu_dropped_operation_count,
        "dropped operation count");
    add(aggregate.persisted_gpu_operation_count,
        chunk.persisted_gpu_operation_count, "operation count");
    add(aggregate.persisted_gpu_accepted_operation_count,
        chunk.persisted_gpu_accepted_operation_count,
        "accepted operation count");
    add(aggregate.persisted_gpu_job_count,
        chunk.persisted_gpu_job_count, "operation job count");
    add(aggregate.persisted_gpu_accepted_job_count,
        chunk.persisted_gpu_accepted_job_count,
        "accepted operation job count");
    aggregate.measured_region_ns = checked_logical_u64_sum(
        aggregate.measured_region_ns, chunk.measured_region_ns,
        "measured algorithm");
    aggregate.validation_excluded_ns = checked_logical_u64_sum(
        aggregate.validation_excluded_ns, chunk.validation_excluded_ns,
        "excluded validation");
    aggregate.peak_postgres_rss_kb = std::max(
        aggregate.peak_postgres_rss_kb, chunk.peak_postgres_rss_kb);
    aggregate.peak_minimap2_rss_kb = std::max(
        aggregate.peak_minimap2_rss_kb, chunk.peak_minimap2_rss_kb);
    aggregate.peak_bench_rss_kb = std::max(
        aggregate.peak_bench_rss_kb, chunk.peak_bench_rss_kb);
    if (aggregate.min_tmp_available_kb == 0) {
        aggregate.min_tmp_available_kb = chunk.min_tmp_available_kb;
    } else if (chunk.min_tmp_available_kb != 0) {
        aggregate.min_tmp_available_kb = std::min(
            aggregate.min_tmp_available_kb, chunk.min_tmp_available_kb);
    }
    aggregate.peak_swap_used_mb = std::max(
        aggregate.peak_swap_used_mb, chunk.peak_swap_used_mb);
    append_shifted_interval_origins(
        aggregate.benchmark_e2e_interval_origins,
        chunk.benchmark_e2e_interval_origins, range.begin);
    aggregate.pipeline_stats_available = std::min(
        aggregate.pipeline_stats_available, chunk.pipeline_stats_available);
    aggregate.pipeline_stats_attached_to_measurement = std::min(
        aggregate.pipeline_stats_attached_to_measurement,
        chunk.pipeline_stats_attached_to_measurement);
    aggregate.pipeline_stats_consistent = std::min(
        aggregate.pipeline_stats_consistent,
        chunk.pipeline_stats_consistent);
    aggregate.stage_engine_matrix_valid = std::min(
        aggregate.stage_engine_matrix_valid,
        chunk.stage_engine_matrix_valid);
    aggregate.persisted_gpu_operation_stats_accepted = std::min(
        aggregate.persisted_gpu_operation_stats_accepted,
        chunk.persisted_gpu_operation_stats_accepted);
    aggregate.persisted_gpu_operation_stats_complete = std::min(
        aggregate.persisted_gpu_operation_stats_complete,
        chunk.persisted_gpu_operation_stats_complete);
    aggregate.persisted_gpu_operation_stats_saturated = std::max(
        aggregate.persisted_gpu_operation_stats_saturated,
        chunk.persisted_gpu_operation_stats_saturated);
    aggregate.stats_saturated = std::max(
        aggregate.stats_saturated, chunk.stats_saturated);
}

void execute_paired_chunked_bioseq(
        const parameter_map& params,
        const e2e_input& in,
        e2e_output& out,
        const std::string_view algorithm,
        const std::string_view pipeline_mode,
        const std::string_view minimizer_index,
        const std::string& table,
        const std::string& projection,
        const int fair_workers,
        const int minimap_threads) {
    const auto chunks = make_logical_query_chunks(
        in.queries.size(), in.logical_query_chunk_size);
    e2e_output setup_output = out;
    it::rowset_t logical_rows;
    logical_rows.reserve(in.queries.size());
    nlohmann::json receipts = nlohmann::json::array();
    // A logical repeat has one cold-start boundary, matching the unchunked
    // just-index execution it replaces. Physical query chunks share that
    // exact restarted backend and READY generation; restarting once per chunk
    // would multiply cold archive/index load work into the logical duration.
    configure_bioseq_algorithm_matrix_legacy_session(
        *in.conn, in, params, /*build_target=*/false);
    capture_pre_restart_persisted_audit(
        *in.conn, table, minimizer_index,
        just_index_sequence_rowno_for(in, table), in,
        setup_output.requested_minimizer_algorithm, setup_output);
    setup_output.just_index_restart_performed =
        restart_postgres_for_cold_just_index(in, setup_output) ? 1 : 0;
    configure_bioseq_scheduler_session(*in.conn, pipeline_mode);
    configure_bioseq_pair_sort_session(*in.conn, params);
    configure_bioseq_algorithm_matrix_legacy_session(
        *in.conn, in, params, /*build_target=*/false);
    it::exec_sql(*in.conn,
        "SET bioseq.index.sequence_read.enabled = on;");
    it::exec_sql(
        *in.conn,
        e2e_expects_compact_h32_direct(
            in, setup_output.requested_minimizer_algorithm, minimizer_index)
            ? "SET bioseq.aligner.fragment_minimizer.lookup_route = "
                "'direct_compact_hash_lookup';"
            : "SET bioseq.aligner.fragment_minimizer.lookup_route = "
                "'control_block_scan';");
    bool first = true;
    for (std::size_t ordinal = 0; ordinal < chunks.size(); ++ordinal) {
        const logical_query_chunk chunk = chunks[ordinal];
        e2e_input chunk_input = in;
        chunk_input.queries.assign(
            in.queries.begin() + static_cast<std::ptrdiff_t>(chunk.begin),
            in.queries.begin() + static_cast<std::ptrdiff_t>(chunk.end));
        e2e_output chunk_output = setup_output;
        load_e2e_query_table_range(
            *in.conn, in, chunk.begin, chunk.end);
        const alignment_snapshot_identity prior_alignment =
            capture_prior_alignment_identity(*in.conn);
        scoped_e2e_statement_timeout statement_timeout{
            *in.conn, in.statement_timeout_ms};
        const std::string sql = batched_coord_sql(table, projection, 2);
        scoped_resource_monitor resources;
        const auto start_ns = static_cast<std::uint64_t>(
            bioseq::instrumentation::clock::now_ns());
        std::optional<pending_captured_query_result> pending_result;
        {
            auto _scope = bioseq::instrumentation::region(k_measured_region);
            pending_result.emplace(
                execute_query_capturing_result(*in.conn, sql));
        }
        const auto end_ns = static_cast<std::uint64_t>(
            bioseq::instrumentation::clock::now_ns());
        const auto validation_begin_ns = end_ns;
        attach_resource_metrics(
            chunk_output, start_ns, end_ns, resources.stop());
        captured_query_rows captured = materialize_captured_query_rows(
            *in.conn, std::move(*pending_result));
        const bool timeout_restored = statement_timeout.restore();
        chunk_output.logical_statement_timeout_restored =
            timeout_restored ? 1 : 0;
        if (captured.status != captured_query_status::tuples_ok
            || !timeout_restored) {
            chunk_output.sql_result_status = captured.result_status;
            chunk_output.sqlstate = captured.sqlstate;
            chunk_output.sql_primary = captured.primary;
            chunk_output.sql_detail = captured.detail;
            const std::string failure = fmt::format(
                "status={} SQLSTATE={} primary={} detail={} timeout_restored={}",
                captured_query_status_name(captured.status),
                captured.sqlstate, captured.primary, captured.detail,
                timeout_restored);
            record_logical_chunk_failure(
                chunk_output, algorithm, chunk, failure);
            receipts.push_back(nlohmann::json{
                {"ordinal", ordinal},
                {"query_begin", chunk.begin},
                {"query_end", chunk.end},
                {"query_count", chunk.size()},
                {"statement_timeout_ms", in.statement_timeout_ms},
                {"statement_timeout_restored", timeout_restored},
                {"measured_algorithm_ns", chunk_output.measured_region_ns},
                {"tuple_accepted", false},
                {"sqlstate", captured.sqlstate},
                {"primary", captured.primary},
                {"detail", captured.detail},
            });
            out = std::move(chunk_output);
            out.logical_chunk_receipts = nlohmann::json{
                {"schema", "bioseq.e2e.logical-postgres-chunk-receipt"},
                {"version", 1},
                {"physical_chunks", std::move(receipts)},
            };
            out.logical_chunking_enabled = 1;
            out.logical_chunk_count = static_cast<std::int64_t>(ordinal + 1u);
            out.logical_chunk_size = static_cast<std::int64_t>(
                in.logical_query_chunk_size);
            out.logical_statement_timeout_ms = static_cast<std::int64_t>(
                in.statement_timeout_ms);
            out.logical_statement_timeout_restored =
                timeout_restored ? 1 : 0;
            return;
        }
        chunk_output.sql_result_status = captured.result_status;
        capture_alignment_statement_stats(
            *in.conn, prior_alignment, chunk_output);
        capture_post_restart_persisted_audit(
            *in.conn, table, minimizer_index,
            just_index_sequence_rowno_for(in, table), in,
            chunk_output.requested_minimizer_algorithm, chunk_output);
        capture_index_audit(*in.conn, table, minimizer_index, chunk_output);
        finalize_post_measure_persisted_policy_audit(
            minimizer_index, chunk_output);
        const bool pipeline_valid = require_pipeline_execution_stats(
            algorithm, chunk_input, chunk_output,
            make_pipeline_stats_expectation(
                chunk_input, true, fair_workers),
            minimizer_index);
        capture_hash_major_build_stats(*in.conn, chunk_output);
        const auto validation_end_ns = static_cast<std::uint64_t>(
            bioseq::instrumentation::clock::now_ns());
        chunk_output.validation_excluded_ns = validation_end_ns
            >= validation_begin_ns
                ? validation_end_ns - validation_begin_ns : 0;
        if (!pipeline_valid) {
            record_logical_chunk_failure(
                chunk_output, algorithm, chunk,
                "same-statement pipeline/stage receipt rejected");
        }
        receipts.push_back(make_postgres_logical_chunk_receipt(
            chunk_output, chunk, ordinal, in.statement_timeout_ms,
            timeout_restored));
        if (!pipeline_valid) {
            out = std::move(chunk_output);
            out.logical_chunk_receipts = nlohmann::json{
                {"schema", "bioseq.e2e.logical-postgres-chunk-receipt"},
                {"version", 1},
                {"physical_chunks", std::move(receipts)},
            };
            out.logical_chunking_enabled = 1;
            out.logical_chunk_count = static_cast<std::int64_t>(ordinal + 1u);
            out.logical_chunk_size = static_cast<std::int64_t>(
                in.logical_query_chunk_size);
            out.logical_statement_timeout_ms = static_cast<std::int64_t>(
                in.statement_timeout_ms);
            out.logical_statement_timeout_restored =
                timeout_restored ? 1 : 0;
            return;
        }
        logical_rows.insert(
            logical_rows.end(),
            std::make_move_iterator(captured.rows.begin()),
            std::make_move_iterator(captured.rows.end()));
        merge_logical_postgres_chunk(
            out, std::move(chunk_output), chunk, first);
        first = false;
    }
    if (first) {
        throw std::logic_error(
            "logical E2E PostgreSQL plan executed no chunks");
    }
    out.alignment_snapshot_rows.clear();
    out.logical_chunking_enabled = 1;
    out.logical_chunk_count = static_cast<std::int64_t>(chunks.size());
    out.logical_chunk_size = static_cast<std::int64_t>(
        in.logical_query_chunk_size);
    out.logical_statement_timeout_ms = static_cast<std::int64_t>(
        in.statement_timeout_ms);
    out.logical_statement_timeout_restored = 1;
    out.persisted_gpu_operation_stats = nlohmann::json{
        {"schema", "bioseq.e2e.logical-persisted-gpu-operation-aggregate"},
        {"version", 1},
        {"physical_chunk_count", chunks.size()},
        {"operation_count_sum", out.persisted_gpu_operation_count},
        {"accepted_operation_count_sum",
            out.persisted_gpu_accepted_operation_count},
        {"dropped_operation_count_sum",
            out.persisted_gpu_dropped_operation_count},
        {"job_count_sum", out.persisted_gpu_job_count},
        {"accepted_job_count_sum",
            out.persisted_gpu_accepted_job_count},
    };
    out.logical_chunk_receipts = nlohmann::json{
        {"schema", "bioseq.e2e.logical-postgres-chunk-receipt"},
        {"version", 1},
        {"query_count", in.queries.size()},
        {"chunk_size", in.logical_query_chunk_size},
        {"chunk_count", chunks.size()},
        {"statement_timeout_ms", in.statement_timeout_ms},
        {"statement_timeout_restored", true},
        {"cold_restart_count", 1},
        {"shared_postmaster_start_time",
            out.timed_postmaster_start_time},
        {"shared_postgres_backend_pid", out.alignment_backend_pid},
        {"shared_database_oid", out.alignment_database_oid},
        {"shared_persisted_generation_id", out.persisted_generation_id},
        {"deterministic_order", "contiguous_global_query_id_ascending"},
        {"measured_algorithm_ns_sum", out.measured_region_ns},
        {"interval_union_ns_sum", out.benchmark_e2e_interval_union_ns},
        {"physical_chunks", std::move(receipts)},
    };
    const auto parse_begin_ns = static_cast<std::uint64_t>(
        bioseq::instrumentation::clock::now_ns());
    parse_bioseq_batch_rows(logical_rows, in, out);
    reject_invalid_matrix_result(in, out);
    const auto parse_end_ns = static_cast<std::uint64_t>(
        bioseq::instrumentation::clock::now_ns());
    if (parse_end_ns >= parse_begin_ns) {
        out.validation_excluded_ns = checked_logical_u64_sum(
            out.validation_excluded_ns,
            parse_end_ns - parse_begin_ns,
            "excluded row validation");
    }
    log_measured_case(
        algorithm, pipeline_mode, minimap_threads, in, out,
        minimizer_index);
}

[[noreturn]] void abort_gate8_sql_proof(const std::string_view reason) {
    std::fprintf(
        stderr, "[bench_e2e] GATE8-SQL-PROOF-FAIL %.*s\n",
        static_cast<int>(reason.size()), reason.data());
    std::fflush(stderr);
    std::abort();
}

[[nodiscard]] e2e_output initialize_e2e_output(
        const parameter_map& params,
        const e2e_input& in) {
    e2e_output out;
    initialize_ordered_query_oracle(in, out);
    if (in.paired_minimap2_oracle) {
        out.paired_generated_target_content_crc64 =
            in.paired_generated_target_content_crc64;
        out.paired_persisted_target_content_crc64 =
            in.paired_persisted_target_content_crc64;
    }
    if (in.require_nice_attestation) {
        out.priority_target_nice = in.priority_target_nice;
        out.runner_pid = in.runner_pid;
        out.runner_nice = in.runner_nice;
    }
    out.requested_minimizer_algorithm =
        param_string(params, "minimizer_algorithm", "");
    out.requested_chaining_algorithm =
        param_string(params, "chaining_algorithm", "");
    out.requested_base_alignment_algorithm =
        param_string(params, "base_alignment_algorithm", "");
    return out;
}

/** @brief Canonical resolved parameter identity for a matrix cell cache key. */
[[nodiscard]] nlohmann::json matrix_resolved_parameter_identity(
        const parameter_map& params) {
    nlohmann::json result = nlohmann::json::object();
    for (const auto& [name, value] : params) {
        if (value.is_string()) {
            result[name] = std::string{value.as_string()};
        } else if (value.is_integer()) {
            result[name] = value.as_integer();
        } else if (value.is_double()) {
            result[name] = value.as_double();
        } else if (value.is_boolean()) {
            result[name] = value.as_boolean();
        } else {
            abort_gate8_sql_proof(
                "matrix attestation rejection identity has a non-scalar parameter");
        }
    }
    return result;
}

/** @brief Exact process-local identity for measured-rejection suppression. */
[[nodiscard]] std::string matrix_attestation_rejection_cell_key(
        const std::string_view algorithm,
        const parameter_map& params,
        const e2e_input& in,
        const std::string_view minimizer_index,
        const e2e_output& out) {
    const bool persisted = algorithm ==
        bioseq::bench::e2e::detail::algorithm_matrix_just_index;
    if (!persisted && algorithm !=
            bioseq::bench::e2e::detail::algorithm_matrix_no_index) {
        abort_gate8_sql_proof(
            "attestation rejection cache key requested outside matrix scope");
    }
    const std::string target_table = persisted
        ? in.matrix_just_index_tables.at(algorithm_matrix_identity(
            out.requested_minimizer_algorithm, minimizer_index))
        : in.no_index_table;
    return nlohmann::json{
        {"schema", "bioseq.e2e.matrix-attestation-rejection-cell"},
        {"version", 1},
        {"algorithm", std::string{algorithm}},
        {"dataset", nlohmann::json{
            {"key", in.size_label},
            {"alphabet", in.alphabet},
            {"sequence_type", in.seq_type},
            {"target_size_bp", in.target_bp},
            {"kmer_size", in.kmer_size},
            {"window_size", in.window_size},
            {"query_count", in.queries.size()},
        }},
        {"execution", nlohmann::json{
            {"mode", persisted ? "just_index" : "no_index"},
            {"minimizer_index", std::string{minimizer_index}},
            {"requested_algorithms", nlohmann::json{
                {"minimizer", out.requested_minimizer_algorithm},
                {"chaining", out.requested_chaining_algorithm},
                {"base_alignment",
                    out.requested_base_alignment_algorithm},
            }},
            {"pipeline_mode",
                param_string(params, "pipeline_mode", "single_thread")},
            {"target_table", target_table},
            {"query_table", in.matrix_query_table},
            {"statement_timeout_ms", in.statement_timeout_ms},
            {"logical_query_chunk_size", in.logical_query_chunk_size},
            {"algorithm_matrix_measurement_scope",
                in.algorithm_matrix_measurement_scope},
            {"paired_minimap2_oracle", in.paired_minimap2_oracle},
            {"require_nice_attestation", in.require_nice_attestation},
            {"priority_target_nice", in.priority_target_nice},
            {"projections", nlohmann::json{
                {"cigar", in.cigar_projection},
                {"mapq", in.mapq_projection},
                {"prefilter", in.prefilter_enabled},
            }},
            {"correctness", nlohmann::json{
                {"designed_indel_cigar", in.designed_indel_cigar},
                {"designed_many_gap_cigar", in.designed_many_gap_cigar},
                {"require_cigar_indel", in.require_cigar_indel},
                {"min_cigar_deletion_ops", in.min_cigar_deletion_ops},
                {"min_cigar_insertion_ops", in.min_cigar_insertion_ops},
            }},
        }},
        {"resolved_parameters", matrix_resolved_parameter_identity(params)},
    }.dump();
}

[[nodiscard]] bool record_algorithm_matrix_setup_rejection(
        const std::string_view algorithm,
        const e2e_input& in,
        const std::string_view minimizer_index,
        e2e_output& out) {
    const captured_query_rows* captured = nullptr;
    std::string setup_identity;
    bool table_setup_rejection = false;
    bool index_setup_rejection = false;
    bool vulkan_hardware_rejection = false;
    if (algorithm == bioseq::bench::e2e::detail::algorithm_matrix_no_index
        && in.matrix_no_index_setup_rejection.has_value()) {
        captured = &*in.matrix_no_index_setup_rejection;
        setup_identity = algorithm_matrix_identity(
            out.requested_minimizer_algorithm, minimizer_index);
        table_setup_rejection =
            is_allowlisted_algorithm_matrix_table_setup_rejection(*captured, in);
        if (!table_setup_rejection) {
            abort_unexpected_e2e_table_setup_result(
                *captured, in.no_index_table);
        }
    } else if (algorithm ==
            bioseq::bench::e2e::detail::algorithm_matrix_just_index) {
        setup_identity = algorithm_matrix_identity(
            out.requested_minimizer_algorithm, minimizer_index);
        const auto rejected =
            in.matrix_just_index_setup_rejections.find(setup_identity);
        if (rejected == in.matrix_just_index_setup_rejections.end()) {
            return false;
        }
        captured = &rejected->second;
        table_setup_rejection =
            is_allowlisted_algorithm_matrix_table_setup_rejection(*captured, in);
        index_setup_rejection =
            is_allowlisted_algorithm_matrix_setup_rejection(
                *captured, in, out.requested_minimizer_algorithm,
                minimizer_index);
        vulkan_hardware_rejection =
            is_allowlisted_algorithm_matrix_vulkan_hardware_rejection(
                *captured, in, out.requested_minimizer_algorithm,
                minimizer_index);
        if (static_cast<int>(table_setup_rejection)
                + static_cast<int>(index_setup_rejection)
                + static_cast<int>(vulkan_hardware_rejection) != 1) {
            abort_unexpected_e2e_table_setup_result(*captured, setup_identity);
        }
    } else {
        return false;
    }
    if (captured == nullptr) return false;

    out.tuple_accepted = 0;
    out.cell_disposition = "rejected_setup";
    out.phase = "setup";
    out.correctness_evaluated = 0;
    out.expected_match_count = static_cast<std::int64_t>(in.queries.size());
    out.sql_result_status = captured->result_status;
    out.sqlstate = captured->sqlstate;
    out.sql_primary = captured->primary;
    out.sql_detail = captured->detail;
    out.sql_hint = captured->hint;
    out.sql_connection_diagnostic = captured->connection_diagnostic;
    if (table_setup_rejection) {
        out.tuple_rejection_reason = fmt::format(
            "allowlisted untimed bioseq.seq typmod capability rejection "
            "identity={} SQLSTATE={} primary={}",
            setup_identity, captured->sqlstate, captured->primary);
    } else if (index_setup_rejection) {
        out.tuple_rejection_reason = fmt::format(
            "allowlisted untimed bioseq.update_index capability rejection "
            "identity={} SQLSTATE={} primary={}",
            setup_identity, captured->sqlstate, captured->primary);
    } else {
        out.tuple_rejection_reason = fmt::format(
            "allowlisted untimed bioseq.update_index Vulkan hardware "
            "capability rejection identity={} SQLSTATE={} primary={}",
            setup_identity, captured->sqlstate, captured->primary);
    }
    return true;
}

struct gate8_guc_snapshot {
    std::string sql;
    std::string text;
    nlohmann::json values = nullptr;
};

[[nodiscard]] nlohmann::json expected_gate8_gucs(
        const e2e_input& in,
        const parameter_map& params,
        const std::string_view pipeline_mode,
        const std::string_view minimizer_index,
        const bool no_index) {
    const std::string minimizer =
        param_string(params, "minimizer_algorithm", "scalar");
    const bool vulkan_seed = minimizer == "vulkan_minimap_integer";
    return nlohmann::json{
        {"bioseq.planner.custom_scan.enabled", "on"},
        {"bioseq.planner.index_scan.enabled", "on"},
        {"bioseq.vectorization.enabled", "on"},
        {"bioseq.scheduler.pipeline.mode", std::string{pipeline_mode}},
        {"bioseq.aligner.minimizer.algorithm", minimizer},
        {"bioseq.aligner.chaining.algorithm",
            param_string(params, "chaining_algorithm", "scalar_minimap2")},
        {"bioseq.aligner.base_alignment.algorithm",
            param_string(params, "base_alignment_algorithm", "ksw2")},
        {"bioseq.aligner.minimizer.kmer_size", fmt::format("{}", in.kmer_size)},
        {"bioseq.aligner.minimizer.window_size", fmt::format("{}", in.window_size)},
        {"bioseq.aligner.grouped_soa_generation.enabled", "on"},
        {"bioseq.aligner.vulkan.enabled", "on"},
        {"bioseq.aligner.vulkan.seed_lookup.enabled",
            vulkan_seed ? "on" : "off"},
        {"bioseq.aligner.vulkan.index_cache.enabled", "on"},
        {"bioseq.aligner.vulkan.fused_frontend.enabled", "off"},
        {"bioseq.aligner.vulkan.alignment_extension.enabled", "off"},
        {"bioseq.aligner.vulkan.index_build.enabled",
            vulkan_seed && no_index ? "on" : "off"},
        {"bioseq.aligner.fragment_minimizer.lookup_route",
            minimizer_index == k_minimizers_per_fragment_index
                ? "direct_compact_hash_lookup" : "control_block_scan"},
        {"bioseq.column_cache.enabled", "on"},
        {"bioseq.index.async_persistence.enabled", "off"},
        {"bioseq.index.recompute_async_persistence.enabled", "off"},
        {"bioseq.index.insert_maintenance.enabled", "off"},
        {"bioseq.index.sequence_read.enabled", no_index ? "off" : "on"},
    };
}

[[nodiscard]] gate8_guc_snapshot capture_gate8_guc_snapshot(
        const it::connection_test_context& connection,
        const nlohmann::json& expected) {
    static const std::string sql =
        "SELECT json_build_object("
        "'bioseq.planner.custom_scan.enabled', current_setting('bioseq.planner.custom_scan.enabled'), "
        "'bioseq.planner.index_scan.enabled', current_setting('bioseq.planner.index_scan.enabled'), "
        "'bioseq.vectorization.enabled', current_setting('bioseq.vectorization.enabled'), "
        "'bioseq.scheduler.pipeline.mode', current_setting('bioseq.scheduler.pipeline.mode'), "
        "'bioseq.aligner.minimizer.algorithm', current_setting('bioseq.aligner.minimizer.algorithm'), "
        "'bioseq.aligner.chaining.algorithm', current_setting('bioseq.aligner.chaining.algorithm'), "
        "'bioseq.aligner.base_alignment.algorithm', current_setting('bioseq.aligner.base_alignment.algorithm'), "
        "'bioseq.aligner.minimizer.kmer_size', current_setting('bioseq.aligner.minimizer.kmer_size'), "
        "'bioseq.aligner.minimizer.window_size', current_setting('bioseq.aligner.minimizer.window_size'), "
        "'bioseq.aligner.grouped_soa_generation.enabled', current_setting('bioseq.aligner.grouped_soa_generation.enabled'), "
        "'bioseq.aligner.vulkan.enabled', current_setting('bioseq.aligner.vulkan.enabled'), "
        "'bioseq.aligner.vulkan.seed_lookup.enabled', current_setting('bioseq.aligner.vulkan.seed_lookup.enabled'), "
        "'bioseq.aligner.vulkan.index_cache.enabled', current_setting('bioseq.aligner.vulkan.index_cache.enabled'), "
        "'bioseq.aligner.vulkan.fused_frontend.enabled', current_setting('bioseq.aligner.vulkan.fused_frontend.enabled'), "
        "'bioseq.aligner.vulkan.alignment_extension.enabled', current_setting('bioseq.aligner.vulkan.alignment_extension.enabled'), "
        "'bioseq.aligner.vulkan.index_build.enabled', current_setting('bioseq.aligner.vulkan.index_build.enabled'), "
        "'bioseq.aligner.fragment_minimizer.lookup_route', current_setting('bioseq.aligner.fragment_minimizer.lookup_route'), "
        "'bioseq.column_cache.enabled', current_setting('bioseq.column_cache.enabled'), "
        "'bioseq.index.async_persistence.enabled', current_setting('bioseq.index.async_persistence.enabled'), "
        "'bioseq.index.recompute_async_persistence.enabled', current_setting('bioseq.index.recompute_async_persistence.enabled'), "
        "'bioseq.index.insert_maintenance.enabled', current_setting('bioseq.index.insert_maintenance.enabled'), "
        "'bioseq.index.sequence_read.enabled', current_setting('bioseq.index.sequence_read.enabled')"
        ")::text;";
    const it::rowset_t rows = it::query_rows(connection, sql);
    if (rows.size() != 1u || rows.front().size() != 1u
        || !rows.front().front().has_value()) {
        abort_gate8_sql_proof("complete 22-GUC snapshot has an invalid SQL shape");
    }
    gate8_guc_snapshot snapshot{
        .sql = sql,
        .text = *rows.front().front(),
    };
    try {
        snapshot.values = nlohmann::json::parse(snapshot.text);
    } catch (const nlohmann::json::exception&) {
        abort_gate8_sql_proof("complete 22-GUC snapshot is not valid JSON");
    }
    if (!snapshot.values.is_object() || snapshot.values.size() != 22u
        || snapshot.values != expected) {
        abort_gate8_sql_proof("complete 22-GUC snapshot differs from the selected cell");
    }
    return snapshot;
}

[[nodiscard]] nlohmann::json gate8_guc_snapshot_json(
        const gate8_guc_snapshot& snapshot) {
    return nlohmann::json{
        {"sql", snapshot.sql},
        {"json_text", snapshot.text},
        {"values", snapshot.values},
    };
}

[[nodiscard]] nlohmann::json gate8_route_signature(
        const e2e_output& out) {
    return nlohmann::json{
        {"requested_backend", out.requested_backend},
        {"actual_backend", out.actual_backend},
        {"requested_target_index_source", out.requested_target_index_source},
        {"actual_target_index_source", out.actual_target_index_source},
        {"requested_algorithms", nlohmann::json{
            {"minimizer", out.attested_requested_minimizer_algorithm},
            {"chaining", out.attested_requested_chaining_algorithm},
            {"base_alignment", out.attested_requested_base_alignment_algorithm},
        }},
        {"observed_algorithms", nlohmann::json{
            {"minimizer", out.observed_minimizer_algorithm},
            {"chaining", out.observed_chaining_algorithm},
            {"base_alignment", out.observed_base_alignment_algorithm},
        }},
        {"requested_simd_chaining_policy", out.requested_simd_chaining_policy},
        {"observed_simd_chaining_policy", out.observed_simd_chaining_policy},
        {"requested_simd_alignment_algorithm",
            out.requested_simd_alignment_algorithm},
        {"observed_simd_alignment_algorithm",
            out.observed_simd_alignment_algorithm},
        {"prefilter_activity_count", out.prefilter_activity_count},
        {"backend_declines", out.backend_declines},
        {"backend_fallbacks", out.backend_fallbacks},
    };
}

[[nodiscard]] nlohmann::json ordered_primary_match_counts(
        const e2e_output& out) {
    nlohmann::json counts = nlohmann::json::array();
    for (const e2e_query_oracle_observation& observation :
         out.ordered_query_oracle) {
        counts.push_back(observation.primary_match_count);
    }
    return counts;
}

[[nodiscard]] nlohmann::json gate8_cache_evidence(
        const e2e_output& out,
        const bool persisted) {
    const bool persisted_vulkan_seed = persisted
        && out.attested_requested_minimizer_algorithm
            == "vulkan_minimap_integer";
    const bool exact_counters = persisted_vulkan_seed
        ? out.gpu_persisted_resident_uploads == 1
            && out.gpu_persisted_resident_cache_hits == 0
        : out.gpu_persisted_resident_uploads == 0
            && out.gpu_persisted_resident_cache_hits == 0;
    if (!exact_counters) {
        abort_gate8_sql_proof(
            "Gate-8 cache qualification contradicts existing resident counters");
    }
    return nlohmann::json{
        {"applicability", persisted_vulkan_seed
            ? "persisted_vulkan_seed_statement_resident"
            : "not_applicable_to_selected_route"},
        {"not_applicable_reason", persisted_vulkan_seed
            ? nlohmann::json(nullptr)
            : nlohmann::json(
                persisted ? "selected_minimizer_is_not_vulkan_seed"
                          : "no_index_uses_transient_target_construction")},
        {"gpu_persisted_resident_uploads",
            out.gpu_persisted_resident_uploads},
        {"gpu_persisted_resident_cache_hits",
            out.gpu_persisted_resident_cache_hits},
    };
}

[[nodiscard]] nlohmann::json cold_absence_json(
        const cold_no_index_policy_absence_stats& value) {
    return nlohmann::json{
        {"target_rows", value.target_rows},
        {"target_cardinality_metadata_only",
            value.target_cardinality_metadata_only},
        {"source_parent_rows_read", value.source_parent_rows_read},
        {"source_fragment_rows_read", value.source_fragment_rows_read},
        {"source_decoded_bp", value.source_decoded_bp},
        {"physical_index_relations", value.physical_index_relations},
        {"persisted_index_rows", value.persisted_index_rows},
        {"generation_objects", value.generation_objects},
    };
}

[[nodiscard]] nlohmann::json capture_gate8_query_fixture_proof(
        const it::connection_test_context& connection,
        const e2e_input& in) {
    const it::row_t identity = it::query_one(
        connection,
        fmt::format(
            "SELECT count(*)::bigint, count(DISTINCT id)::bigint, "
            "min(id)::bigint, max(id)::bigint, am.amname "
            "FROM {} q CROSS JOIN LATERAL ("
            "SELECT a.amname FROM pg_class c JOIN pg_am a ON a.oid=c.relam "
            "WHERE c.oid='{}'::regclass) am GROUP BY am.amname;",
            in.matrix_query_table, in.matrix_query_table));
    const std::int64_t expected_count =
        static_cast<std::int64_t>(in.queries.size());
    if (identity.size() != 5u
        || row_i64_or_minus_one(identity, 0) != expected_count
        || row_i64_or_minus_one(identity, 1) != expected_count
        || row_i64_or_minus_one(identity, 2) != 0
        || row_i64_or_minus_one(identity, 3) != expected_count - 1
        || !identity[4].has_value() || *identity[4] != "bioseq") {
        abort_gate8_sql_proof("durable query fixture identity changed");
    }
    const it::rowset_t policy_rows = it::query_rows(
        connection,
        fmt::format(
            "SELECT index_name, update_on_insert::int "
            "FROM bioseq_internal.index_policy "
            "WHERE user_relid='{}'::regclass "
            "AND main_relfilenode=(SELECT relfilenode FROM pg_class "
            "WHERE oid='{}'::regclass) "
            "AND column_attno=(SELECT attnum FROM pg_attribute "
            "WHERE attrelid='{}'::regclass AND attname='seq') "
            "ORDER BY index_name;",
            in.matrix_query_table, in.matrix_query_table,
            in.matrix_query_table));
    nlohmann::json policies = nlohmann::json::object();
    for (const it::row_t& row : policy_rows) {
        if (row.size() != 2u || !row[0].has_value()
            || row_i64_or_minus_one(row, 1) != 0
            || (*row[0] != k_minimizers_per_fragment_index
                && *row[0] != k_minimizers_per_sequence_index)
            || policies.contains(*row[0])) {
            abort_gate8_sql_proof(
                "durable query fixture policy is not exactly UPDATE_MANUALLY");
        }
        policies[*row[0]] = "UPDATE_MANUALLY";
    }
    if (policies.size() != 2u) {
        abort_gate8_sql_proof(
            "durable query fixture does not expose both generated policies");
    }
    const auto mpf = read_cold_no_index_policy_absence_stats(
        connection, in.matrix_query_table, k_minimizers_per_fragment_index);
    const auto mps = read_cold_no_index_policy_absence_stats(
        connection, in.matrix_query_table, k_minimizers_per_sequence_index);
    if (!exact_cold_no_index_absence(mpf, expected_count)
        || !exact_cold_no_index_absence(mps, expected_count)) {
        abort_gate8_sql_proof(
            "durable query fixture acquired a generated physical index");
    }
    return nlohmann::json{
        {"table", in.matrix_query_table},
        {"access_method", "bioseq"},
        {"row_count", expected_count},
        {"distinct_query_id_count", expected_count},
        {"minimum_query_id", 0},
        {"maximum_query_id", expected_count - 1},
        {"policies", std::move(policies)},
        {"physical_absence", nlohmann::json{
            {std::string{k_minimizers_per_fragment_index},
                cold_absence_json(mpf)},
            {std::string{k_minimizers_per_sequence_index},
                cold_absence_json(mps)},
        }},
    };
}

struct e2e_matrix_iteration_preparation {
    e2e_output output{};
    std::string algorithm;
    std::string pipeline_mode;
    std::string minimizer_index;
    std::string target_table;
    std::string measured_sql;
    alignment_snapshot_identity prior_alignment{};
    gate8_guc_snapshot measured_gucs{};
    std::unique_ptr<scoped_resource_monitor> resources{};
    std::optional<pending_captured_query_result> pending_result{};
    std::size_t repeat = 0;
    int fair_workers = 0;
    bool persisted = false;
    bool execute_statement = false;
};

struct e2e_minimap2_iteration_preparation {
    e2e_output output{};
    std::string algorithm;
    std::string pipeline_mode;
    std::unique_ptr<scoped_resource_monitor> resources{};
    std::optional<prepared_minimap2_helper_invocation> prepared_helper{};
    std::optional<pending_minimap2_helper_result> pending_result{};
    int minimap_threads = 0;
    bool execute_helper = false;
};

using e2e_iteration_preparation = std::variant<
    e2e_matrix_iteration_preparation,
    e2e_minimap2_iteration_preparation>;

/** @brief Capture the timed SQL session identity for a Gate-8 binding. */
[[nodiscard]] nlohmann::json gate8_current_postgres_identity(
        const e2e_output& measured) {
    return nlohmann::json{
        {"postmaster_start_time", measured.timed_postmaster_start_time},
        {"backend_pid", measured.alignment_backend_pid},
        {"database_oid", measured.alignment_database_oid},
    };
}

/** @brief Project one accepted measurement into Gate-8 evidence. */
[[nodiscard]] nlohmann::json gate8_measurement_evidence(
        const e2e_input& in,
        const e2e_matrix_iteration_preparation& prep,
        const e2e_output& measured) {
    return nlohmann::json{
        {"boundary", "framework_iteration_measurement_window"},
        {"operation", "single_libpq_PQexec_mmsearch_statement"},
        {"statement_count", 1},
        {"sql", prep.measured_sql},
        {"target_table", prep.target_table},
        {"query_table", in.matrix_query_table},
        {"index_read_requirement", prep.persisted ? 2 : 1},
        {"pre_statement_gucs",
            gate8_guc_snapshot_json(prep.measured_gucs)},
        {"ordered_primary_match_counts",
            ordered_primary_match_counts(measured)},
        {"route", gate8_route_signature(measured)},
        {"cache_evidence", gate8_cache_evidence(measured, prep.persisted)},
    };
}

/** @brief Build the process-local identity for one Gate-8 matrix cell. */
[[nodiscard]] nlohmann::json gate8_static_cell_identity(
        const e2e_input& in,
        const e2e_matrix_iteration_preparation& prep,
        const e2e_output& measured) {
    return nlohmann::json{
        {"algorithm", prep.algorithm},
        {"dataset_key", in.size_label},
        {"alphabet", in.alphabet},
        {"target_size_bp", in.target_bp},
        {"kmer_size", in.kmer_size},
        {"window_size", in.window_size},
        {"query_count", in.queries.size()},
        {"mode", prep.persisted ? "just_index" : "no_index"},
        {"minimizer_index", prep.minimizer_index},
        {"requested_algorithms", nlohmann::json{
            {"minimizer", measured.attested_requested_minimizer_algorithm},
            {"chaining", measured.attested_requested_chaining_algorithm},
            {"base_alignment",
                measured.attested_requested_base_alignment_algorithm},
        }},
        {"pipeline_mode", prep.pipeline_mode},
        {"max_concurrency", prep.fair_workers},
        {"target_table", prep.target_table},
        {"query_table", in.matrix_query_table},
        {"index_read_requirement", prep.persisted ? 2 : 1},
        {"projections", nlohmann::json{
            {"cigar", in.cigar_projection},
            {"mapq", in.mapq_projection},
            {"prefilter", in.prefilter_enabled},
        }},
    };
}

/** @brief Complete a Gate-8 cell identity with its observed route and device. */
[[nodiscard]] nlohmann::json gate8_runtime_cell_identity(
        const nlohmann::json& static_identity,
        const e2e_matrix_iteration_preparation& prep,
        const e2e_output& measured) {
    nlohmann::json result = static_identity;
    result["route"] = gate8_route_signature(measured);
    result["target_state"] = nlohmann::json{
        {"kind", prep.persisted
            ? "persisted_generation" : "transient_recomputed"},
        {"layout", prep.persisted
            ? nlohmann::json(measured.persisted_layout)
            : nlohmann::json(nullptr)},
        {"generation_id", prep.persisted
            ? nlohmann::json(measured.persisted_generation_id)
            : nlohmann::json(nullptr)},
        {"source_identity", prep.persisted
            ? nlohmann::json(measured.persisted_source_identity)
            : nlohmann::json(nullptr)},
    };
    result["device"] = nlohmann::json{
        {"execution_device", measured.actual_backend == "hybrid_cpu_vulkan"
            ? "cpu_and_gpu" : "cpu"},
        {"gpu_device_uuid", measured.gpu_device_uuid},
        {"gpu_driver", measured.gpu_driver},
    };
    return result;
}

[[nodiscard]] bool use_e2e_matrix_iteration_boundaries(
        const std::string_view algorithm,
        const e2e_input& in) noexcept {
    return !in.paired_minimap2_oracle
        && (algorithm ==
                bioseq::bench::e2e::detail::algorithm_matrix_just_index
            || algorithm ==
                bioseq::bench::e2e::detail::algorithm_matrix_no_index);
}

[[nodiscard]] bool use_e2e_minimap2_iteration_boundaries(
        const std::string_view algorithm,
        const e2e_input& in) noexcept {
    return !in.paired_minimap2_oracle
        && (algorithm == "minimap2_just_index"
            || algorithm == "minimap2_no_index");
}

[[nodiscard]] bool use_e2e_iteration_boundaries(
        const std::string_view algorithm,
        const e2e_input& in) noexcept {
    return in.algorithm_matrix_measurement_scope
        && (use_e2e_matrix_iteration_boundaries(algorithm, in)
            || use_e2e_minimap2_iteration_boundaries(algorithm, in));
}

[[nodiscard]] e2e_matrix_iteration_preparation
prepare_e2e_matrix_iteration(
        const std::string_view algorithm,
        const parameter_map& params,
        const e2e_input& in,
        const std::size_t repeat) {
    if (!use_e2e_matrix_iteration_boundaries(algorithm, in)) {
        throw std::invalid_argument(
            "matrix iteration preparation was invoked for an ineligible row");
    }
    e2e_matrix_iteration_preparation prep;
    prep.repeat = repeat;
    prep.output = initialize_e2e_output(params, in);
    prep.algorithm = std::string{algorithm};
    prep.pipeline_mode =
        param_string(params, "pipeline_mode", "single_thread");
    prep.minimizer_index = param_minimizer_index(params);
    prep.persisted = algorithm ==
        bioseq::bench::e2e::detail::algorithm_matrix_just_index;
    if (record_algorithm_matrix_setup_rejection(
            algorithm, in, prep.minimizer_index, prep.output)) {
        return prep;
    }
    const std::string rejection_cell_key =
        matrix_attestation_rejection_cell_key(
            algorithm, params, in, prep.minimizer_index, prep.output);
    const auto prior_rejection =
        in.matrix_measured_attestation_rejections.find(rejection_cell_key);
    if (prior_rejection != in.matrix_measured_attestation_rejections.end()) {
        if (prior_rejection->second.source_repeat >= repeat
            || prior_rejection->second.rejection_reason.empty()) {
            abort_gate8_sql_proof(
                "matrix attestation rejection cache has invalid chronology");
        }
        prep.output.tuple_accepted = 0;
        prep.output.cell_disposition =
            "skipped_after_prior_attestation_rejection";
        prep.output.phase = "not_executed";
        prep.output.correctness_evaluated = 0;
        prep.output.expected_match_count =
            static_cast<std::int64_t>(in.queries.size());
        prep.output.prior_attestation_rejection_source_repeat =
            static_cast<std::int64_t>(
                prior_rejection->second.source_repeat);
        prep.output.prior_attestation_rejection_reason =
            prior_rejection->second.rejection_reason;
        prep.output.tuple_rejection_reason =
            prior_rejection->second.rejection_reason;
        return prep;
    }
    if (in.matrix_query_table.empty()) {
        abort_gate8_sql_proof(
            "accepted non-paired matrix row has no durable query fixture");
    }
    prep.fair_workers = fair_scheduler_worker_count(*in.conn);
    const int configured_minimap_threads = param_int(params, "threads", 0);
    if (configured_minimap_threads > 0
        && configured_minimap_threads != prep.fair_workers) {
        abort_gate8_sql_proof(
            "configured minimap thread count differs from scheduler workers");
    }
    prep.output.comparison_cpu_workers = prep.fair_workers;
    if (prep.persisted) {
        const std::string identity = algorithm_matrix_identity(
            prep.output.requested_minimizer_algorithm,
            prep.minimizer_index);
        prep.target_table = in.matrix_just_index_tables.at(identity);
        configure_bioseq_algorithm_matrix_session(
            *in.conn, in, params, prep.pipeline_mode,
            prep.minimizer_index, /*build_target=*/false);
        capture_pre_restart_persisted_audit(
            *in.conn, prep.target_table, prep.minimizer_index,
            just_index_sequence_rowno_for(in, prep.target_table), in,
            prep.output.requested_minimizer_algorithm, prep.output);
        prep.output.just_index_restart_performed =
            restart_postgres_for_cold_just_index(in, prep.output) ? 1 : 0;
        configure_bioseq_scheduler_session(*in.conn, prep.pipeline_mode);
        configure_bioseq_pair_sort_session(*in.conn, params);
        configure_bioseq_algorithm_matrix_session(
            *in.conn, in, params, prep.pipeline_mode,
            prep.minimizer_index, /*build_target=*/false);
    } else {
        prep.target_table = in.no_index_table;
        const bool recompute_async_persistence =
            param_string(params, "recompute_async_persistence", "off") == "on";
        if (recompute_async_persistence) {
            abort_gate8_sql_proof(
                "standard no-index matrix rows forbid async recomputation");
        }
        prepare_and_restart_cold_no_index_base(
            in, prep.output, prep.minimizer_index, prep.pipeline_mode);
        configure_bioseq_scheduler_session(*in.conn, prep.pipeline_mode);
        configure_bioseq_pair_sort_session(*in.conn, params);
        configure_bioseq_algorithm_matrix_session(
            *in.conn, in, params, prep.pipeline_mode,
            prep.minimizer_index, /*build_target=*/true);
        configure_bioseq_minimizer_policy(
            *in.conn, in.no_index_table, prep.minimizer_index);
        configure_no_index_storage_session(*in.conn);
        capture_cold_no_index_absence(
            *in.conn, in, prep.output,
            cold_no_index_audit_stage::post_restart);
    }
    const nlohmann::json expected_gucs = expected_gate8_gucs(
        in, params, prep.pipeline_mode, prep.minimizer_index,
        !prep.persisted);
    prep.measured_gucs = capture_gate8_guc_snapshot(*in.conn, expected_gucs);
    prep.measured_sql = batched_coord_sql(
        prep.target_table, e2e_projection_sql(in),
        prep.persisted ? 2 : 1, in.matrix_query_table);
    prep.prior_alignment = capture_prior_alignment_identity(*in.conn);
    // The monitor starts after every setup/proof query and before the framework
    // takes t_begin. after_iteration stops it after framework t_end and attaches
    // the framework-owned window, not a second hand-rolled timer.
    prep.resources = std::make_unique<scoped_resource_monitor>();
    prep.execute_statement = true;
    return prep;
}

[[nodiscard]] e2e_output measure_e2e_matrix_iteration(
        const std::string_view,
        const parameter_map&,
        const e2e_input& in,
        e2e_matrix_iteration_preparation& prep) {
    if (!prep.execute_statement) return std::move(prep.output);
    {
        auto measured_scope =
            bioseq::instrumentation::region(k_measured_region);
        prep.pending_result.emplace(
            execute_query_capturing_result(*in.conn, prep.measured_sql));
    }
    return std::move(prep.output);
}

[[nodiscard]] e2e_minimap2_iteration_preparation
prepare_e2e_minimap2_iteration(
        const std::string_view algorithm,
        const parameter_map& params,
        const e2e_input& in,
        const std::size_t) {
    if (!use_e2e_minimap2_iteration_boundaries(algorithm, in)) {
        throw std::invalid_argument(
            "minimap2 iteration preparation was invoked for an ineligible row");
    }
    e2e_minimap2_iteration_preparation prep;
    prep.output = initialize_e2e_output(params, in);
    prep.algorithm = std::string{algorithm};
    prep.pipeline_mode =
        param_string(params, "pipeline_mode", "single_thread");
    prep.minimap_threads = fair_scheduler_worker_count(*in.conn);
    const int configured_minimap_threads = param_int(params, "threads", 0);
    if (configured_minimap_threads > 0
        && configured_minimap_threads != prep.minimap_threads) {
        throw std::runtime_error(
            "configured minimap2 worker count differs from live scheduler workers");
    }
    prep.output.comparison_cpu_workers = prep.minimap_threads;
    const bool just_index = algorithm == "minimap2_just_index";
    prep.prepared_helper.emplace(prepare_minimap2_helper(
        just_index ? minimap2_receipt_mode::prebuilt_index
                   : minimap2_receipt_mode::no_index,
        prep.minimap_threads, in.kmer_size, in.window_size,
        in.queries.size(), in.minimap2_index_parts,
        just_index ? std::string_view{in.mmi_path}
                   : std::string_view{in.target_fasta_split},
        in.query_fasta, /*preserve_receipt=*/true,
        std::chrono::duration_cast<std::chrono::milliseconds>(
            k_minimap2_child_timeout)));
    // Start resource observation only after workspace/token/argv ownership is
    // complete and immediately before the framework takes t_begin.
    prep.resources = std::make_unique<scoped_resource_monitor>();
    prep.execute_helper = true;
    return prep;
}

[[nodiscard]] e2e_output measure_e2e_minimap2_iteration(
        const std::string_view,
        const parameter_map&,
        const e2e_input&,
        e2e_minimap2_iteration_preparation& prep) {
    if (!prep.execute_helper) return std::move(prep.output);
    if (!prep.prepared_helper.has_value()) {
        throw std::runtime_error("prepared minimap2 helper state is absent");
    }
    {
        auto measured_scope =
            bioseq::instrumentation::region(k_measured_region);
        prep.pending_result.emplace(execute_prepared_minimap2_helper(
            *prep.prepared_helper));
    }
    return std::move(prep.output);
}

[[nodiscard]] nlohmann::json run_gate8_control(
        it::connection_test_context& connection,
        const e2e_input& in,
        const parameter_map& params,
        const std::string_view pipeline_mode,
        const std::string_view minimizer_index,
        const std::string_view expected_persisted_layout,
        const bool no_index,
        const std::string& measured_sql,
        const nlohmann::json& accepted_route,
        const nlohmann::json& accepted_counts,
        const nlohmann::json& accepted_cache_evidence,
        const std::string_view name,
        const std::span<const std::string_view> changed_settings,
        nlohmann::json control_expected) {
    if (name == "cache_off") {
        control_expected["bioseq.aligner.vulkan.index_cache.enabled"] = "off";
        control_expected["bioseq.column_cache.enabled"] = "off";
    } else if (name == "vectorization_off") {
        control_expected["bioseq.vectorization.enabled"] = "off";
    } else {
        abort_gate8_sql_proof("unknown Gate-8 control identity");
    }
    std::fprintf(stderr, "[bench_e2e] GATE8-PHASE-BEGIN control=%.*s queries=%zu\n",
                 static_cast<int>(name.size()), name.data(), in.queries.size());
    std::fflush(stderr);
    const auto restore_control_state = [&]() {
        configure_bioseq_algorithm_matrix_session(
            connection, in, params, pipeline_mode, minimizer_index,
            /*build_target=*/no_index);
    };
    try {
        for (const std::string_view statement : changed_settings) {
            it::exec_sql(connection, statement);
        }
        gate8_guc_snapshot control_gucs =
            capture_gate8_guc_snapshot(connection, control_expected);
        e2e_output control_output = initialize_e2e_output(params, in);
        control_output.pre_restart_persisted_layout =
            std::string{expected_persisted_layout};
        const alignment_snapshot_identity prior =
            capture_prior_alignment_identity(connection);
        captured_query_rows captured;
        captured = query_rows_capturing_error(connection, measured_sql);
        if (captured.status == captured_query_status::tuples_ok) {
            control_output.sql_result_status = captured.result_status;
            parse_bioseq_batch_rows(captured.rows, in, control_output);
            capture_alignment_statement_stats(
                connection, prior, control_output,
                /*enforce_measured_envelope=*/false);
        }
        restore_control_state();
        const nlohmann::json accepted_expected = expected_gate8_gucs(
            in, params, pipeline_mode, minimizer_index, no_index);
        const gate8_guc_snapshot restored =
            capture_gate8_guc_snapshot(connection, accepted_expected);
        if (captured.status != captured_query_status::tuples_ok) {
            std::fprintf(stderr,
                         "[bench_e2e] GATE8-CONTROL-SQL-FAIL control=%.*s status=%s sqlstate=%s primary=%s\n",
                         static_cast<int>(name.size()), name.data(),
                         captured.result_status.c_str(), captured.sqlstate.c_str(),
                         captured.primary.c_str());
            std::fflush(stderr);
            abort_gate8_sql_proof(
                "negative control did not complete the unchanged mapping SQL");
        }
        const nlohmann::json route = gate8_route_signature(control_output);
        const nlohmann::json counts =
            ordered_primary_match_counts(control_output);
        const nlohmann::json cache_evidence = gate8_cache_evidence(
            control_output, /*persisted=*/!no_index);
        if (!control_output.found || route != accepted_route
            || counts != accepted_counts
            || cache_evidence != accepted_cache_evidence
            || control_output.prefilter_activity_count != 0) {
            std::fprintf(stderr,
                         "[bench_e2e] GATE8-CONTROL-CONTRACT-FAIL control=%.*s found=%d route_equal=%d counts_equal=%d cache_equal=%d prefilter_activity=%llu\n",
                         static_cast<int>(name.size()), name.data(),
                         control_output.found ? 1 : 0, route == accepted_route ? 1 : 0,
                         counts == accepted_counts ? 1 : 0,
                         cache_evidence == accepted_cache_evidence ? 1 : 0,
                         static_cast<unsigned long long>(control_output.prefilter_activity_count));
            std::fflush(stderr);
            abort_gate8_sql_proof(
                "negative control changed route, ordered output, or prefilter activity");
        }
        std::fprintf(stderr, "[bench_e2e] GATE8-PHASE-PASS control=%.*s\n",
                     static_cast<int>(name.size()), name.data());
        std::fflush(stderr);
        nlohmann::json changed = nlohmann::json::array();
        if (name == "cache_off") {
            changed.push_back("bioseq.aligner.vulkan.index_cache.enabled=off");
            changed.push_back("bioseq.column_cache.enabled=off");
        } else {
            changed.push_back("bioseq.vectorization.enabled=off");
        }
        return nlohmann::json{
            {"identity", name},
            {"publishable", false},
            {"disposition", "nonpublishable_control_config_changed"},
            {"changed_settings", std::move(changed)},
            {"gucs", gate8_guc_snapshot_json(control_gucs)},
            {"result_status", captured.result_status},
            {"ordered_primary_match_counts", std::move(counts)},
            {"route", std::move(route)},
            {"cache_evidence", std::move(cache_evidence)},
            {"restored_gucs", gate8_guc_snapshot_json(restored)},
        };
    } catch (...) {
        try {
            restore_control_state();
        } catch (...) {
            // The detached proof connection is discarded by the caller. Keep
            // the original qualification failure as the authoritative error.
        }
        throw;
    }
}

struct gate8_explain_contract_accumulator {
    std::size_t batch_join_nodes = 0;
    std::size_t matching_pipeline_groups = 0;
    nlohmann::json pipeline = nullptr;
};

[[nodiscard]] std::string_view gate8_chaining_display_name(
        const std::string_view identity) {
    using bioseq::simd::chaining_algorithm;
    using bioseq::simd::chaining_algorithm_name;
    if (identity == "scalar_minimap2")
        return chaining_algorithm_name(chaining_algorithm::scalar_minimap2);
    if (identity == "simd_minimap2")
        return chaining_algorithm_name(chaining_algorithm::simd_minimap2);
    if (identity == "range_minimum_query")
        return chaining_algorithm_name(chaining_algorithm::range_minimum_query);
    if (identity == "vulkan_tiled_dynamic_programming")
        return chaining_algorithm_name(chaining_algorithm::vulkan_tiled_dynamic_programming);
    abort_gate8_sql_proof("unknown chaining identity in EXPLAIN contract");
}

[[nodiscard]] std::string_view gate8_base_alignment_display_name(
        const std::string_view identity) {
    using bioseq::simd::base_alignment_algorithm;
    using bioseq::simd::base_alignment_algorithm_name;
    if (identity == "ksw2")
        return base_alignment_algorithm_name(base_alignment_algorithm::ksw2);
    if (identity == "block_aligner")
        return base_alignment_algorithm_name(base_alignment_algorithm::block_aligner);
    abort_gate8_sql_proof("unknown base-alignment identity in EXPLAIN contract");
}

void collect_gate8_explain_contract(
        const nlohmann::json& value,
        const std::string_view expected_chaining,
        const std::string_view expected_base,
        const std::string_view expected_concurrency,
        const int expected_workers,
        gate8_explain_contract_accumulator& result) {
    if (value.is_object()) {
        const auto node_type = value.find("Node Type");
        const auto provider = value.find("Custom Plan Provider");
        const bool is_batch_join = node_type != value.end() && provider != value.end()
            && node_type->is_string() && provider->is_string()
            && *node_type == "Custom Scan"
            && *provider == "BioseqBatchJoin";
        if (is_batch_join) {
            ++result.batch_join_nodes;
        }
        const auto pipeline_it = value.find("Bioseq Pipeline");
        // A child scan's pipeline cannot attest the join owner's frozen plan.
        if (is_batch_join && pipeline_it != value.end() && pipeline_it->is_object()) {
            const nlohmann::json& pipeline = *pipeline_it;
            const auto chaining = pipeline.find("Chaining Algorithm");
            const auto base = pipeline.find("Base Alignment Algorithm");
            const auto concurrency =
                pipeline.find("Pipeline Concurrency Mode");
            const auto workers = pipeline.find("Max Concurrency");
            if (chaining != pipeline.end() && chaining->is_string()
                && base != pipeline.end() && base->is_string()
                && concurrency != pipeline.end() && concurrency->is_string()
                && workers != pipeline.end()
                && (workers->is_number_unsigned()
                    || (workers->is_number_integer()
                        && workers->get<std::int64_t>() >= 0))
                && chaining->get_ref<const std::string&>()
                    == gate8_chaining_display_name(expected_chaining)
                && base->get_ref<const std::string&>()
                    == gate8_base_alignment_display_name(expected_base)
                && concurrency->get_ref<const std::string&>()
                    == expected_concurrency
                && workers->get<std::uint64_t>()
                    == static_cast<std::uint64_t>(expected_workers)) {
                ++result.matching_pipeline_groups;
                result.pipeline = nlohmann::json{
                    {"chaining_algorithm", std::string{expected_chaining}},
                    {"base_alignment_algorithm", std::string{expected_base}},
                    {"pipeline_concurrency_mode", *concurrency},
                    {"max_concurrency", *workers},
                };
            }
        }
        for (const auto& [name, child] : value.items()) {
            (void)name;
            collect_gate8_explain_contract(
                child, expected_chaining, expected_base,
                expected_concurrency, expected_workers, result);
        }
    } else if (value.is_array()) {
        for (const nlohmann::json& child : value) {
            collect_gate8_explain_contract(
                child, expected_chaining, expected_base,
                expected_concurrency, expected_workers, result);
        }
    }
}

[[nodiscard]] nlohmann::json gate8_explain_contract(
        const std::string_view explain_json_text,
        const e2e_matrix_iteration_preparation& prep,
        const e2e_output& measured) {
    nlohmann::json document;
    try {
        document = nlohmann::json::parse(explain_json_text);
    } catch (const nlohmann::json::exception&) {
        abort_gate8_sql_proof(
            "literal EXPLAIN ANALYZE bytes are not valid JSON");
    }
    gate8_explain_contract_accumulator result;
    collect_gate8_explain_contract(
        document, measured.attested_requested_chaining_algorithm,
        measured.attested_requested_base_alignment_algorithm,
        prep.pipeline_mode, prep.fair_workers, result);
    if (result.batch_join_nodes != 1u
        || result.matching_pipeline_groups != 1u
        || result.pipeline.is_null()) {
        abort_gate8_sql_proof(
            "literal EXPLAIN ANALYZE does not prove exactly one "
            "BioseqBatchJoin and its frozen chaining/base/concurrency plan");
    }
    return nlohmann::json{
        {"custom_scan_node_count", result.batch_join_nodes},
        {"node_type", "Custom Scan"},
        {"custom_plan_provider", "BioseqBatchJoin"},
        {"matching_pipeline_group_count", result.matching_pipeline_groups},
        {"pipeline", std::move(result.pipeline)},
    };
}

/** @brief Qualify one measured Gate-8 cell with detached SQL guards. */
[[nodiscard]] nlohmann::json capture_gate8_cell_qualification(
        const e2e_input& in,
        const parameter_map& params,
        const e2e_matrix_iteration_preparation& prep,
        const e2e_output& measured,
        const nlohmann::json& runtime_cell,
        const nlohmann::json& source_measurement,
        const nlohmann::json& source_postgres,
        const nlohmann::json& source_query_fixture) {
    it::connection_test_context proof_connection;
    const postmaster_identity measured_identity =
        prep.persisted
            ? postmaster_identity{
                .start_time = measured.post_restart_postmaster_start_time,
                .backend_pid = measured.alignment_backend_pid,
                .database_oid = measured.alignment_database_oid,
            }
            : postmaster_identity{
                .start_time =
                    measured.cold_no_index_post_restart_postmaster_start_time,
                .backend_pid = measured.alignment_backend_pid,
                .database_oid = measured.alignment_database_oid,
            };
    const nlohmann::json measured_identity_json{
        {"postmaster_start_time", measured_identity.start_time},
        {"backend_pid", measured_identity.backend_pid},
        {"database_oid", measured_identity.database_oid},
    };
    if (source_postgres != measured_identity_json) {
        abort_gate8_sql_proof(
            "Gate-8 source witness differs from its measured SQL session");
    }
    const postmaster_identity proof_identity =
        capture_postmaster_identity(proof_connection);
    if (proof_identity.database_oid != measured_identity.database_oid
        || proof_identity.start_time != measured_identity.start_time
        || proof_identity.backend_pid <= 0
        || proof_identity.backend_pid == measured_identity.backend_pid) {
        abort_gate8_sql_proof(
            "detached proof is not same-database/same-postmaster/different-PID");
    }
    configure_bioseq_scheduler_session(proof_connection, prep.pipeline_mode);
    configure_bioseq_pair_sort_session(proof_connection, params);
    configure_bioseq_algorithm_matrix_session(
        proof_connection, in, params, prep.pipeline_mode,
        prep.minimizer_index, /*build_target=*/!prep.persisted);
    const nlohmann::json expected_gucs = expected_gate8_gucs(
        in, params, prep.pipeline_mode, prep.minimizer_index,
        !prep.persisted);
    const gate8_guc_snapshot accepted_gucs =
        capture_gate8_guc_snapshot(proof_connection, expected_gucs);

    const alignment_snapshot_identity explain_prior =
        capture_prior_alignment_identity(proof_connection);
    const std::string explain_sql =
        "EXPLAIN (ANALYZE, FORMAT JSON) " + prep.measured_sql;
    std::fprintf(stderr, "[bench_e2e] GATE8-PHASE-BEGIN control=explain_analyze queries=%zu\n",
                 in.queries.size());
    std::fflush(stderr);
    const it::rowset_t explain_rows =
        it::query_rows(proof_connection, explain_sql);
    if (explain_rows.size() != 1u || explain_rows.front().size() != 1u
        || !explain_rows.front().front().has_value()) {
        abort_gate8_sql_proof("literal EXPLAIN ANALYZE returned no JSON document");
    }
    const std::string explain_json_text = *explain_rows.front().front();
    e2e_output explain_stats = initialize_e2e_output(params, in);
    explain_stats.pre_restart_persisted_layout =
        measured.pre_restart_persisted_layout;
    capture_alignment_statement_stats(
        proof_connection, explain_prior, explain_stats,
        /*enforce_measured_envelope=*/false);
    e2e_output explain_plan;
    simdjson::dom::parser parser;
    simdjson::dom::element document;
    if (parser.parse(explain_json_text).get(document) != simdjson::SUCCESS) {
        abort_gate8_sql_proof("literal EXPLAIN ANALYZE JSON failed to parse");
    }
    const nlohmann::json explain_contract = gate8_explain_contract(
        explain_json_text, prep, measured);
    pipeline_stats_parse_state parse_state{};
    walk_pipeline_explain_json(document, explain_plan, parse_state);
    if (!parse_state.group_found || parse_state.invalid
        || !pipeline_stats_fields_complete(explain_plan)) {
        abort_gate8_sql_proof(
            "literal EXPLAIN ANALYZE lacks one complete execution-stat group");
    }
    const nlohmann::json measured_route = gate8_route_signature(measured);
    const nlohmann::json explain_route = gate8_route_signature(explain_stats);
    if (explain_route != measured_route
        || explain_plan.requested_backend != measured.requested_backend
        || explain_plan.actual_backend != measured.actual_backend
        || explain_plan.requested_target_index_source
            != measured.requested_target_index_source
        || explain_plan.actual_target_index_source
            != measured.actual_target_index_source
        || explain_stats.prefilter_activity_count != 0) {
        abort_gate8_sql_proof(
            "literal EXPLAIN ANALYZE changed the selected route");
    }
    std::fprintf(stderr, "[bench_e2e] GATE8-PHASE-PASS control=explain_analyze\n");
    std::fflush(stderr);
    const nlohmann::json measured_counts =
        ordered_primary_match_counts(measured);
    const nlohmann::json measured_cache_evidence =
        gate8_cache_evidence(measured, prep.persisted);
    const std::array<std::string_view, 2> cache_off{{
        "SET bioseq.aligner.vulkan.index_cache.enabled = off;",
        "SET bioseq.column_cache.enabled = off;",
    }};
    const std::array<std::string_view, 1> vectorization_off{{
        "SET bioseq.vectorization.enabled = off;",
    }};
    const nlohmann::json cache_control = run_gate8_control(
        proof_connection, in, params, prep.pipeline_mode,
        prep.minimizer_index, measured.pre_restart_persisted_layout,
        !prep.persisted, prep.measured_sql,
        measured_route, measured_counts, measured_cache_evidence,
        "cache_off", cache_off,
        expected_gucs);
    const nlohmann::json vectorization_control = run_gate8_control(
        proof_connection, in, params, prep.pipeline_mode,
        prep.minimizer_index, measured.pre_restart_persisted_layout,
        !prep.persisted, prep.measured_sql,
        measured_route, measured_counts, measured_cache_evidence,
        "vectorization_off",
        vectorization_off, expected_gucs);
    return nlohmann::json{
        {"runtime_cell", runtime_cell},
        {"source_repeat", prep.repeat},
        {"source_postgres", source_postgres},
        {"source_measurement", source_measurement},
        {"query_fixture", source_query_fixture},
        {"detached", nlohmann::json{
            {"same_database", true},
            {"same_postmaster", true},
            {"different_backend_pid", true},
            {"measured_backend_pid", measured_identity.backend_pid},
            {"proof_backend_pid", proof_identity.backend_pid},
            {"database_oid", proof_identity.database_oid},
            {"postmaster_start_time", proof_identity.start_time},
            {"accepted_gucs", gate8_guc_snapshot_json(accepted_gucs)},
            {"explain_sql", explain_sql},
            {"explain_json_text", explain_json_text},
            {"explain_contract", explain_contract},
            {"explain_route", explain_route},
            {"controls", nlohmann::json{
                {"cache_off", cache_control},
                {"vectorization_off", vectorization_control},
            }},
        }},
    };
}

/** @brief Bind an accepted repeat to its cell's source qualification. */
[[nodiscard]] nlohmann::json bind_gate8_sql_proof(
        const e2e_input& in,
        const parameter_map& params,
        const e2e_matrix_iteration_preparation& prep,
        const e2e_output& measured) {
    const nlohmann::json static_cell =
        gate8_static_cell_identity(in, prep, measured);
    const nlohmann::json runtime_cell =
        gate8_runtime_cell_identity(static_cell, prep, measured);
    const nlohmann::json current_measurement =
        gate8_measurement_evidence(in, prep, measured);
    const nlohmann::json current_postgres =
        gate8_current_postgres_identity(measured);
    // The measured alignment statistics are already fixed before this
    // metadata-only fixture guard runs on the current timed connection.
    const nlohmann::json current_query_fixture =
        capture_gate8_query_fixture_proof(*in.conn, in);
    const std::string qualification_key = static_cell.dump();

    auto qualification_it =
        in.matrix_gate8_cell_qualifications.find(qualification_key);
    bool is_source = false;
    if (qualification_it == in.matrix_gate8_cell_qualifications.end()) {
        if (prep.repeat != 0u) {
            abort_gate8_sql_proof(
                "Gate-8 cell qualification source is not repeat zero");
        }
        // The registry is mutated only after EXPLAIN, both controls, control
        // restoration, and fixture validation all completed successfully.
        nlohmann::json qualification = capture_gate8_cell_qualification(
            in, params, prep, measured, runtime_cell,
            current_measurement, current_postgres, current_query_fixture);
        nlohmann::json entry{
            {"qualification", std::move(qualification)},
            {"bound_repeats", nlohmann::json::array({prep.repeat})},
            {"bound_postgres",
                nlohmann::json::array({current_postgres})},
        };
        const auto [inserted_it, inserted] =
            in.matrix_gate8_cell_qualifications.emplace(
                qualification_key, std::move(entry));
        if (!inserted) {
            abort_gate8_sql_proof(
                "Gate-8 cell qualification was registered concurrently");
        }
        qualification_it = inserted_it;
        is_source = true;
    }

    nlohmann::json& entry = qualification_it->second;
    if (!entry.is_object() || !entry.contains("qualification")
        || !entry["qualification"].is_object()
        || !entry.contains("bound_repeats")
        || !entry["bound_repeats"].is_array()
        || !entry.contains("bound_postgres")
        || !entry["bound_postgres"].is_array()) {
        abort_gate8_sql_proof(
            "Gate-8 cell qualification registry entry is malformed");
    }
    const nlohmann::json& qualification = entry["qualification"];
    if (qualification.value("source_repeat", std::size_t{1}) != 0u
        || !qualification.contains("runtime_cell")
        || qualification["runtime_cell"] != runtime_cell
        || !qualification.contains("source_measurement")
        || qualification["source_measurement"] != current_measurement
        || !qualification.contains("source_postgres")
        || !qualification["source_postgres"].is_object()
        || !qualification.contains("query_fixture")
        || qualification["query_fixture"] != current_query_fixture) {
        abort_gate8_sql_proof(
            "Gate-8 reuse does not match its source cell, route, measurement, "
            "or current query fixture");
    }
    const nlohmann::json& source_postgres =
        qualification["source_postgres"];
    if (source_postgres.value("database_oid", std::int64_t{-1}) <= 0
        || source_postgres.value("backend_pid", std::int64_t{-1}) <= 0
        || source_postgres.value("postmaster_start_time", std::string{}).empty()
        || current_postgres.value("database_oid", std::int64_t{-1})
            != source_postgres.value("database_oid", std::int64_t{-1})
        || current_postgres.value("backend_pid", std::int64_t{-1}) <= 0
        || current_postgres.value(
            "postmaster_start_time", std::string{}).empty()) {
        abort_gate8_sql_proof(
            "Gate-8 qualification has an invalid PostgreSQL identity");
    }

    if (is_source) {
        if (current_postgres != source_postgres
            || entry["bound_repeats"].size() != 1u
            || entry["bound_postgres"].size() != 1u) {
            abort_gate8_sql_proof(
                "Gate-8 source binding does not own its true SQL session");
        }
    } else {
        if (prep.repeat == 0u
            || current_postgres.value(
                "postmaster_start_time", std::string{})
                == source_postgres.value(
                    "postmaster_start_time", std::string{})) {
            abort_gate8_sql_proof(
                "Gate-8 reuse did not follow a distinct cold PostgreSQL restart");
        }
        for (const nlohmann::json& bound_repeat : entry["bound_repeats"]) {
            if (!bound_repeat.is_number_unsigned()
                || bound_repeat.get<std::size_t>() == prep.repeat) {
                abort_gate8_sql_proof(
                    "Gate-8 repeat was rebound or registry history is malformed");
            }
        }
        for (const nlohmann::json& bound_postgres : entry["bound_postgres"]) {
            if (!bound_postgres.is_object()
                || bound_postgres.value(
                    "postmaster_start_time", std::string{})
                    == current_postgres.value(
                        "postmaster_start_time", std::string{})) {
                abort_gate8_sql_proof(
                    "Gate-8 cold PostgreSQL epoch was replayed");
            }
        }
        entry["bound_repeats"].push_back(prep.repeat);
        entry["bound_postgres"].push_back(current_postgres);
    }

    return nlohmann::json{
        {"schema", "bioseq.e2e.gate8-sql-proof"},
        {"version", 2},
        {"scope", "accepted_nonpaired_bioseq_algorithm_matrix"},
        {"qualification_order", "after_source_measurement"},
        {"measurement", current_measurement},
        {"query_fixture", current_query_fixture},
        {"binding", nlohmann::json{
            {"repeat", prep.repeat},
            {"source_repeat", qualification["source_repeat"]},
            {"role", is_source ? "source" : "reuse"},
            {"source_epoch_is_current", is_source},
            {"current_postgres", current_postgres},
            {"source_postgres", source_postgres},
        }},
        {"qualification", qualification},
    };
}

void finalize_e2e_matrix_iteration(
        const std::string_view algorithm,
        const parameter_map& params,
        const e2e_input& in,
        e2e_matrix_iteration_preparation& prep,
        e2e_output& out,
        const std::int64_t begin_ns,
        const std::int64_t end_ns,
        const std::uint64_t elapsed_ns) {
    if (!prep.execute_statement) return;
    if (begin_ns < 0 || end_ns < begin_ns
        || static_cast<std::uint64_t>(end_ns - begin_ns) != elapsed_ns
        || prep.resources == nullptr || !prep.pending_result.has_value()) {
        abort_gate8_sql_proof("framework measurement window is malformed");
    }
    attach_resource_metrics(
        out, static_cast<std::uint64_t>(begin_ns),
        static_cast<std::uint64_t>(end_ns), prep.resources->stop());
    prep.resources.reset();
    captured_query_rows captured = materialize_captured_query_rows(
        *in.conn, std::move(*prep.pending_result));
    prep.pending_result.reset();
    if (captured.status != captured_query_status::tuples_ok) {
        if (captured.status == captured_query_status::connection_failure) {
            const auto recovery =
                reconnect_and_prove_measured_query_connection_failure(
                    in, measured_query_session_identity(out, prep.persisted));
            if (!recovery.has_value()) {
                abort_unexpected_algorithm_matrix_query_result(captured, out);
            }
            record_measured_query_connection_failure(
                captured, in, *recovery, out);
            return;
        }
        if (!is_allowlisted_algorithm_matrix_sql_rejection(
                captured, in, out, prep.persisted)) {
            abort_unexpected_algorithm_matrix_query_result(captured, out);
        }
        record_allowlisted_algorithm_matrix_sql_rejection(
            captured, in, out);
        return;
    }
    out.sql_result_status = captured.result_status;
    parse_bioseq_batch_rows(captured.rows, in, out);
    finalize_ordered_query_oracle(in, out);
    reject_invalid_matrix_result(in, out);
    // This remains the first SQL after result materialization/CPU validation;
    // no diagnostic mapping can overwrite the measured statement's one-shot
    // operation ledger.
    capture_alignment_statement_stats(*in.conn, prep.prior_alignment, out);
    if (prep.persisted) {
        capture_post_restart_persisted_audit(
            *in.conn, prep.target_table, prep.minimizer_index,
            just_index_sequence_rowno_for(in, prep.target_table), in,
            out.requested_minimizer_algorithm, out);
        capture_index_audit(
            *in.conn, prep.target_table, prep.minimizer_index, out);
        finalize_post_measure_persisted_policy_audit(
            prep.minimizer_index, out);
    } else {
        capture_hash_major_build_stats(*in.conn, out);
        capture_index_audit(
            *in.conn, in.no_index_table, prep.minimizer_index, out);
        capture_cold_no_index_absence(
            *in.conn, in, out, cold_no_index_audit_stage::measured);
        require_cold_no_index_lifecycle(out);
    }
    (void)require_pipeline_execution_stats(
        algorithm, in, out,
        make_pipeline_stats_expectation(
            in, prep.persisted, prep.fair_workers),
        prep.minimizer_index);
    if (prep.persisted) capture_hash_major_build_stats(*in.conn, out);
    // Coverage campaign: validate the measured statement above, without three
    // extra full mapping executions (EXPLAIN ANALYZE/cache-off/vectorization-off).
    // Leave gate8_sql null: absent control evidence must never be fabricated.
    log_measured_case(
        algorithm, prep.pipeline_mode, prep.fair_workers, in, out,
        prep.minimizer_index);
    finalize_e2e_priority_attestation(algorithm, in, out);
    annotate_accepted_behavior_differences(in, out);
    if (out.cell_disposition == "rejected_attestation") {
        if (out.tuple_accepted != 0 || out.phase != "attestation"
            || out.tuple_rejection_reason.empty()) {
            abort_gate8_sql_proof(
                "measured matrix attestation rejection is malformed");
        }
        const std::string rejection_cell_key =
            matrix_attestation_rejection_cell_key(
                algorithm, params, in, prep.minimizer_index, out);
        const bool inserted =
            in.matrix_measured_attestation_rejections.emplace(
                rejection_cell_key,
                e2e_matrix_attestation_rejection{
                    .source_repeat = prep.repeat,
                    .rejection_reason = out.tuple_rejection_reason,
                }).second;
        if (!inserted) {
            abort_gate8_sql_proof(
                "matrix attestation rejection cache has duplicate source");
        }
    }
}

void finalize_e2e_minimap2_iteration(
        const std::string_view algorithm,
        const parameter_map&,
        const e2e_input& in,
        e2e_minimap2_iteration_preparation& prep,
        e2e_output& out,
        const std::int64_t begin_ns,
        const std::int64_t end_ns,
        const std::uint64_t elapsed_ns) {
    if (!prep.execute_helper) return;
    if (begin_ns < 0 || end_ns < begin_ns
        || static_cast<std::uint64_t>(end_ns - begin_ns) != elapsed_ns
        || prep.resources == nullptr || !prep.pending_result.has_value()) {
        throw std::runtime_error(
            "framework minimap2 measurement window is malformed");
    }
    attach_resource_metrics(
        out, static_cast<std::uint64_t>(begin_ns),
        static_cast<std::uint64_t>(end_ns), prep.resources->stop());
    prep.resources.reset();
    minimap2_helper_result helper = finalize_minimap2_helper(
        std::move(*prep.pending_result));
    prep.pending_result.reset();
    parse_minimap2_paf(helper.stdout_text, in, out);
    attach_minimap2_receipt(out, helper);
    finalize_ordered_query_oracle(in, out);
    require_minimap2_union_within_manual_envelope(out);
    reject_invalid_matrix_result(in, out);
    log_measured_case(
        algorithm, prep.pipeline_mode, prep.minimap_threads, in, out);
    finalize_e2e_priority_attestation(algorithm, in, out);
    annotate_accepted_behavior_differences(in, out);
}

[[nodiscard]] e2e_iteration_preparation prepare_e2e_iteration(
        const std::string_view algorithm,
        const parameter_map& params,
        const e2e_input& in,
        const std::size_t repeat) {
    if (use_e2e_matrix_iteration_boundaries(algorithm, in)) {
        return prepare_e2e_matrix_iteration(
            algorithm, params, in, repeat);
    }
    if (use_e2e_minimap2_iteration_boundaries(algorithm, in)) {
        return prepare_e2e_minimap2_iteration(
            algorithm, params, in, repeat);
    }
    throw std::invalid_argument(
        "iteration boundary preparation was invoked for an ineligible row");
}

[[nodiscard]] e2e_output measure_e2e_iteration(
        const std::string_view algorithm,
        const parameter_map& params,
        const e2e_input& in,
        e2e_iteration_preparation& preparation) {
    if (auto* matrix = std::get_if<e2e_matrix_iteration_preparation>(
            &preparation)) {
        return measure_e2e_matrix_iteration(
            algorithm, params, in, *matrix);
    }
    return measure_e2e_minimap2_iteration(
        algorithm, params, in,
        std::get<e2e_minimap2_iteration_preparation>(preparation));
}

void finalize_e2e_iteration(
        const std::string_view algorithm,
        const parameter_map& params,
        const e2e_input& in,
        e2e_iteration_preparation& preparation,
        e2e_output& out,
        const bioseq::bench::framework::iteration_measurement_window& window) {
    if (auto* matrix = std::get_if<e2e_matrix_iteration_preparation>(
            &preparation)) {
        finalize_e2e_matrix_iteration(
            algorithm, params, in, *matrix, out,
            window.begin_ns, window.end_ns, window.elapsed_ns);
        return;
    }
    finalize_e2e_minimap2_iteration(
        algorithm, params, in,
        std::get<e2e_minimap2_iteration_preparation>(preparation), out,
        window.begin_ns, window.end_ns, window.elapsed_ns);
}

[[nodiscard]] e2e_output
e2e_run(std::string_view algo, const parameter_map& params, const e2e_input& in) {
    // The sliced case exercises the full SIMD pipeline. Block Aligner executes
    // only when a downstream result helper requests CIGAR data.
    if (!is_sliced_pipeline_stats_configuration_valid(algo, in.cigar_projection)) {
        std::fprintf(stderr,
                     "[bench_e2e] SLICED-PIPELINE-CONFIG-FAIL "
                     "algo=%.*s requires data.cigar_projection=true so the "
                     "BlockAligner stage is executed and validated\n",
                     static_cast<int>(algo.size()),
                     algo.data());
        std::fflush(stderr);
        std::abort();
    }
    e2e_output out = initialize_e2e_output(params, in);
    const std::string pipeline_mode =
        param_string(params, "pipeline_mode", "single_thread");
    const std::string minimizer_index = param_minimizer_index(params);
    if (algo == bioseq::bench::e2e::detail::algorithm_matrix_no_index
        && in.matrix_no_index_setup_rejection.has_value()) {
        const captured_query_rows& captured =
            *in.matrix_no_index_setup_rejection;
        if (!is_allowlisted_algorithm_matrix_table_setup_rejection(
                captured, in)) {
            abort_unexpected_e2e_table_setup_result(
                captured, in.no_index_table);
        }
        out.tuple_accepted = 0;
        out.cell_disposition = "rejected_setup";
        out.phase = "setup";
        out.correctness_evaluated = 0;
        out.expected_match_count =
            static_cast<std::int64_t>(in.queries.size());
        out.sql_result_status = captured.result_status;
        out.sqlstate = captured.sqlstate;
        out.sql_primary = captured.primary;
        out.sql_detail = captured.detail;
        out.sql_hint = captured.hint;
        out.sql_connection_diagnostic = captured.connection_diagnostic;
        out.tuple_rejection_reason = fmt::format(
            "allowlisted untimed bioseq.seq typmod capability rejection "
            "identity={} SQLSTATE={} primary={}",
            algorithm_matrix_identity(
                out.requested_minimizer_algorithm,
                minimizer_index),
            captured.sqlstate,
            captured.primary);
        return out;
    }
    if (algo == bioseq::bench::e2e::detail::algorithm_matrix_just_index) {
        const std::string setup_identity = algorithm_matrix_identity(
            out.requested_minimizer_algorithm, minimizer_index);
        const auto rejected =
            in.matrix_just_index_setup_rejections.find(setup_identity);
        if (rejected != in.matrix_just_index_setup_rejections.end()) {
            const captured_query_rows& captured = rejected->second;
            const bool table_setup_rejection =
                is_allowlisted_algorithm_matrix_table_setup_rejection(
                    captured, in);
            const bool index_setup_rejection =
                is_allowlisted_algorithm_matrix_setup_rejection(
                    captured,
                    in,
                    out.requested_minimizer_algorithm,
                    minimizer_index);
            const bool vulkan_hardware_rejection =
                is_allowlisted_algorithm_matrix_vulkan_hardware_rejection(
                    captured,
                    in,
                    out.requested_minimizer_algorithm,
                    minimizer_index);
            const int matched_rejection_count =
                static_cast<int>(table_setup_rejection)
                + static_cast<int>(index_setup_rejection)
                + static_cast<int>(vulkan_hardware_rejection);
            if (matched_rejection_count != 1) {
                abort_unexpected_e2e_table_setup_result(captured, setup_identity);
            }
            out.tuple_accepted = 0;
            out.cell_disposition = "rejected_setup";
            out.phase = "setup";
            out.correctness_evaluated = 0;
            out.expected_match_count =
                static_cast<std::int64_t>(in.queries.size());
            out.sql_result_status = captured.result_status;
            out.sqlstate = captured.sqlstate;
            out.sql_primary = captured.primary;
            out.sql_detail = captured.detail;
            out.sql_hint = captured.hint;
            out.sql_connection_diagnostic = captured.connection_diagnostic;
            out.tuple_rejection_reason = table_setup_rejection
                ? fmt::format(
                    "allowlisted untimed bioseq.seq typmod capability "
                    "rejection identity={} SQLSTATE={} primary={}",
                    setup_identity,
                    captured.sqlstate,
                    captured.primary)
                : index_setup_rejection
                    ? fmt::format(
                    "allowlisted untimed bioseq.update_index capability "
                    "rejection identity={} SQLSTATE={} primary={}",
                    setup_identity,
                    captured.sqlstate,
                    captured.primary)
                    : fmt::format(
                    "allowlisted untimed bioseq.update_index Vulkan hardware "
                    "capability rejection identity={} SQLSTATE={} primary={}",
                    setup_identity,
                    captured.sqlstate,
                    captured.primary);
            return out;
        }
    }
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
    /* Zero means exact parity with the live bioseq scheduler.  Bioseq stats
     * later independently attests the same effective worker count. */
    const int minimap_threads = fair_workers;
    out.comparison_cpu_workers = fair_workers;
    const std::string qlit = fmt::format(
        "'{}'::{}", in.queries.front().bases, in.seq_type);
    const std::string proj = e2e_projection_sql(in);

    if (algo == "bioseq_ksw2_scalar_dp_scalar_mm_just_index") {
        const std::string& table = just_index_table_for(
            in, minimizer_index, just_index_setup_kind::scalar);
        configure_bioseq_scalar_session(
            *in.conn, in.kmer_size, in.window_size);
        capture_pre_restart_persisted_audit(
            *in.conn, table, minimizer_index,
            just_index_sequence_rowno_for(in, table), in, "scalar", out);
        out.just_index_restart_performed =
            restart_postgres_for_cold_just_index(in, out) ? 1 : 0;
        configure_bioseq_scheduler_session(*in.conn, pipeline_mode);
        configure_bioseq_pair_sort_session(*in.conn, params);
        configure_bioseq_scalar_session(
            *in.conn, in.kmer_size, in.window_size);
        it::exec_sql(*in.conn, "SET bioseq.index.sequence_read.enabled = on;");
        it::exec_sql(
            *in.conn,
            "SET bioseq.aligner.fragment_minimizer.lookup_route = "
            "'control_block_scan';");
        const std::string sql = batched_coord_sql(table, proj, 2);
        const alignment_snapshot_identity prior_alignment =
            capture_prior_alignment_identity(*in.conn);
        scoped_resource_monitor resources;
        const auto start_ns = static_cast<std::uint64_t>(
            bioseq::instrumentation::clock::now_ns());
        {
            auto _scope = bioseq::instrumentation::region(k_measured_region);
            load_e2e_query_table(*in.conn, in);
            parse_bioseq_batch_rows(
                it::query_rows(*in.conn, sql), in, out);
        }
        const auto end_ns = static_cast<std::uint64_t>(
            bioseq::instrumentation::clock::now_ns());
        attach_resource_metrics(out, start_ns, end_ns, resources.stop());
        capture_alignment_statement_stats(
            *in.conn, prior_alignment, out);
        capture_post_restart_persisted_audit(
            *in.conn, table, minimizer_index,
            just_index_sequence_rowno_for(in, table), in, "scalar", out);
        capture_index_audit(*in.conn, table, minimizer_index, out);
        finalize_post_measure_persisted_policy_audit(minimizer_index, out);
        (void)require_pipeline_execution_stats(
            algo, in, out,
            make_pipeline_stats_expectation(in, true, fair_workers),
            minimizer_index);
        require_mapped(algo, in, out);
        capture_hash_major_build_stats(*in.conn, out);
        log_measured_case(
            algo, pipeline_mode, minimap_threads, in, out,
            minimizer_index);

    } else if (algo == "bioseq_just_index"
        || algo == "bioseq_block_simd_dp_two_stack_nthash_just_index"
        || algo == bioseq::bench::e2e::detail::algorithm_matrix_just_index) {
        const bool matrix_tuple =
            algo == bioseq::bench::e2e::detail::algorithm_matrix_just_index;
        const std::string_view persisted_requested_minimizer_algorithm =
            matrix_tuple ? std::string_view{out.requested_minimizer_algorithm}
                         : std::string_view{"two_stack_nthash"};
        const std::string matrix_identity = algorithm_matrix_identity(
            out.requested_minimizer_algorithm, minimizer_index);
        const std::string& table = matrix_tuple
            ? in.matrix_just_index_tables.at(matrix_identity)
            : just_index_table_for(in, minimizer_index, just_index_setup_kind::simd);
        if (matrix_tuple && in.paired_minimap2_oracle) {
            execute_paired_chunked_bioseq(
                params, in, out, algo, pipeline_mode, minimizer_index,
                table, proj, fair_workers, minimap_threads);
            finalize_e2e_priority_attestation(algo, in, out);
            annotate_accepted_behavior_differences(in, out);
            return out;
        }
        if (matrix_tuple) {
            configure_bioseq_algorithm_matrix_session(
                *in.conn, in, params, pipeline_mode, minimizer_index,
                /*build_target=*/false);
        } else {
            configure_bioseq_simd_session(*in.conn, in.kmer_size, in.window_size);
        }
        capture_pre_restart_persisted_audit(
            *in.conn, table, minimizer_index,
            just_index_sequence_rowno_for(in, table), in,
            persisted_requested_minimizer_algorithm, out);
        // Restart PostgreSQL first so the prebuilt index (built untimed in
        // on_input) is assembled cold from storage rather than served warm.
        out.just_index_restart_performed =
            restart_postgres_for_cold_just_index(in, out) ? 1 : 0;
        configure_bioseq_scheduler_session(*in.conn, pipeline_mode);
        configure_bioseq_pair_sort_session(*in.conn, params);
        if (matrix_tuple) {
            configure_bioseq_algorithm_matrix_session(
                *in.conn, in, params, pipeline_mode, minimizer_index,
                /*build_target=*/false);
        } else {
            configure_bioseq_simd_session(*in.conn, in.kmer_size, in.window_size);
        }
        it::exec_sql(*in.conn, "SET bioseq.index.sequence_read.enabled = on;");
        // Select the grouped-SoA direct lookup after the cold restart for a
        // per-fragment generation; per-sequence generations use control blocks.
        it::exec_sql(
            *in.conn,
            e2e_expects_compact_h32_direct(
                in, persisted_requested_minimizer_algorithm, minimizer_index)
                ? "SET bioseq.aligner.fragment_minimizer.lookup_route = "
                    "'direct_compact_hash_lookup';"
                : "SET bioseq.aligner.fragment_minimizer.lookup_route = "
                    "'control_block_scan';");
        const std::string sql = batched_coord_sql(table, proj, 2);
        const alignment_snapshot_identity prior_alignment =
            capture_prior_alignment_identity(*in.conn);
        scoped_resource_monitor resources;
        const auto start_ns = static_cast<std::uint64_t>(bioseq::instrumentation::clock::now_ns());
        {
            auto _scope = bioseq::instrumentation::region(k_measured_region);
            load_e2e_query_table(*in.conn, in);
            if (matrix_tuple) {
                captured_query_rows captured =
                    query_rows_capturing_error(*in.conn, sql);
                if (captured.status != captured_query_status::tuples_ok) {
                    if (captured.status
                        == captured_query_status::connection_failure) {
                        (void)resources.stop();
                        const auto recovery =
                            reconnect_and_prove_measured_query_connection_failure(
                                in,
                                measured_query_session_identity(
                                    out, /*persisted_tuple=*/true));
                        if (!recovery.has_value()) {
                            abort_unexpected_algorithm_matrix_query_result(
                                captured, out);
                        }
                        record_measured_query_connection_failure(
                            captured, in, *recovery, out);
                        return out;
                    }
                    if (!is_allowlisted_algorithm_matrix_sql_rejection(
                            captured, in, out, /*persisted_tuple=*/true)) {
                        abort_unexpected_algorithm_matrix_query_result(
                            captured, out);
                    }
                    (void)resources.stop();
                    record_allowlisted_algorithm_matrix_sql_rejection(
                        captured, in, out);
                    return out;
                }
                out.sql_result_status = captured.result_status;
                parse_bioseq_batch_rows(captured.rows, in, out);
            } else {
                parse_bioseq_batch_rows(
                    it::query_rows(*in.conn, sql), in, out);
            }
        }
        const auto end_ns = static_cast<std::uint64_t>(bioseq::instrumentation::clock::now_ns());
        attach_resource_metrics(out, start_ns, end_ns, resources.stop());
        capture_alignment_statement_stats(
            *in.conn, prior_alignment, out);
        capture_post_restart_persisted_audit(
            *in.conn, table, minimizer_index,
            just_index_sequence_rowno_for(in, table), in,
            persisted_requested_minimizer_algorithm, out);
        capture_index_audit(*in.conn, table, minimizer_index, out);
        finalize_post_measure_persisted_policy_audit(minimizer_index, out);
        (void)require_pipeline_execution_stats(
            algo, in, out,
            make_pipeline_stats_expectation(in, true, fair_workers),
            minimizer_index);
        if (out.tuple_accepted == 1) {
            reject_invalid_matrix_result(in, out);
        }
        capture_hash_major_build_stats(*in.conn, out);
        log_measured_case(
            algo, pipeline_mode, minimap_threads, in, out, minimizer_index);

    } else if (algo == "bioseq_block_vulkan_resident_gpu_index_frontend_just_index"
               || algo == "bioseq_block_gpu_dp_gpu_seed_just_index") {
        const std::string& table =
            just_index_table_for(in, minimizer_index, just_index_setup_kind::gpu);
        configure_bioseq_vulkan_index_session(
            *in.conn,
            in.kmer_size,
            in.window_size,
            in.cigar_projection);
        capture_pre_restart_persisted_audit(
            *in.conn, table, minimizer_index,
            just_index_sequence_rowno_for(in, table), in,
            "vulkan_minimap_integer", out);
        // Resident two-submit Vulkan frontend applied to the just-index path:
        // the index is prebuilt (untimed), and only the Vulkan-driven
        // alignment is timed. Use the alignment benchmark's Vulkan settings
        // bioseq_block_vulkan_resident_gpu_index_frontend row.
        // Restart PostgreSQL first so the prebuilt index is assembled cold from
        // storage (GPU seeding/tiled-DP chaining + block aligner over a cold index).
        out.just_index_restart_performed =
            restart_postgres_for_cold_just_index(in, out) ? 1 : 0;
        configure_bioseq_scheduler_session(*in.conn, pipeline_mode);
        configure_bioseq_pair_sort_session(*in.conn, params);
        it::exec_sql(*in.conn, "SET bioseq.index.sequence_read.enabled = on;");
        configure_bioseq_vulkan_index_session(
            *in.conn,
            in.kmer_size,
            in.window_size,
            in.cigar_projection);
        const std::string sql = batched_coord_sql(table, proj, 2);
        const alignment_snapshot_identity prior_alignment =
            capture_prior_alignment_identity(*in.conn);
        scoped_resource_monitor resources;
        const auto start_ns = static_cast<std::uint64_t>(bioseq::instrumentation::clock::now_ns());
        {
            auto _scope = bioseq::instrumentation::region(k_measured_region);
            load_e2e_query_table(*in.conn, in);
            parse_bioseq_batch_rows(it::query_rows(*in.conn, sql), in, out);
        }
        const auto end_ns = static_cast<std::uint64_t>(bioseq::instrumentation::clock::now_ns());
        attach_resource_metrics(out, start_ns, end_ns, resources.stop());
        capture_alignment_statement_stats(
            *in.conn, prior_alignment, out);
        capture_post_restart_persisted_audit(
            *in.conn, table, minimizer_index,
            just_index_sequence_rowno_for(in, table), in,
            "vulkan_minimap_integer", out);
        capture_index_audit(*in.conn, table, minimizer_index, out);
        finalize_post_measure_persisted_policy_audit(minimizer_index, out);
        (void)require_pipeline_execution_stats(
            algo, in, out,
            make_pipeline_stats_expectation(in, true, fair_workers),
            minimizer_index);
        require_mapped(algo, in, out);
        capture_hash_major_build_stats(*in.conn, out);
        log_measured_case(algo, pipeline_mode, minimap_threads, in, out, minimizer_index);

    } else if (algo == "bioseq_ksw2_scalar_dp_scalar_mm_no_index") {
        const bool recompute_async_persistence =
            param_string(params, "recompute_async_persistence", "off") == "on";
        if (recompute_async_persistence) {
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
            *in.conn, in, out, cold_no_index_audit_stage::post_restart);
        const std::string align_sql =
            batched_coord_sql(in.no_index_table, proj, 1);
        const alignment_snapshot_identity prior_alignment =
            capture_prior_alignment_identity(*in.conn);
        scoped_resource_monitor resources;
        const auto start_ns = static_cast<std::uint64_t>(
            bioseq::instrumentation::clock::now_ns());
        {
            auto _scope = bioseq::instrumentation::region(k_measured_region);
            load_e2e_query_table(*in.conn, in);
            parse_bioseq_batch_rows(
                it::query_rows(*in.conn, align_sql), in, out);
        }
        const auto end_ns = static_cast<std::uint64_t>(
            bioseq::instrumentation::clock::now_ns());
        attach_resource_metrics(out, start_ns, end_ns, resources.stop());
        capture_alignment_statement_stats(
            *in.conn, prior_alignment, out);
        capture_hash_major_build_stats(*in.conn, out);
        capture_index_audit(
            *in.conn, in.no_index_table, minimizer_index, out);
        capture_cold_no_index_absence(
            *in.conn, in, out, cold_no_index_audit_stage::measured);
        require_cold_no_index_lifecycle(out);
        (void)require_pipeline_execution_stats(
            algo, in, out,
            make_pipeline_stats_expectation(in, false, fair_workers),
            minimizer_index);
        require_mapped(algo, in, out);
        log_measured_case(
            algo, pipeline_mode, minimap_threads, in, out,
            minimizer_index);

    } else if (algo == "bioseq_no_index"
               || algo == "bioseq_block_simd_dp_two_stack_nthash_no_index"
               || algo == bioseq::bench::e2e::detail::algorithm_matrix_no_index
               || algo == bioseq::bench::e2e::detail::sliced_no_index_algorithm) {
        const bool matrix_tuple =
            algo == bioseq::bench::e2e::detail::algorithm_matrix_no_index;
        const bool recompute_async_persistence =
            param_string(params, "recompute_async_persistence", "off") == "on";
        if (recompute_async_persistence) {
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
        if (!matrix_tuple) {
            configure_bioseq_simd_session(*in.conn, in.kmer_size, in.window_size);
        }
        configure_bioseq_scheduler_session(*in.conn, pipeline_mode);
        configure_bioseq_pair_sort_session(*in.conn, params);
        if (matrix_tuple) {
            configure_bioseq_algorithm_matrix_session(
                *in.conn, in, params, pipeline_mode, minimizer_index,
                /*build_target=*/true);
        }
        configure_bioseq_minimizer_policy(
            *in.conn, in.no_index_table, minimizer_index);
        configure_no_index_storage_session(*in.conn);
        capture_cold_no_index_absence(
            *in.conn, in, out, cold_no_index_audit_stage::post_restart);
        const bool transformed_recipe =
            algo == bioseq::bench::e2e::detail::sliced_no_index_algorithm;
        const std::int64_t transformed_target_origin = transformed_recipe
            ? sliced_transformed_target_origin(in) : 0;
        const std::string align_sql = transformed_recipe
            ? batched_transformed_coord_sql(
                in.no_index_table, proj, 1, transformed_target_origin)
            : batched_coord_sql(in.no_index_table, proj, 1);
        const alignment_snapshot_identity prior_alignment =
            capture_prior_alignment_identity(*in.conn);
        scoped_resource_monitor resources;
        const auto start_ns = static_cast<std::uint64_t>(bioseq::instrumentation::clock::now_ns());
        {
            auto _scope = bioseq::instrumentation::region(k_measured_region);
            load_e2e_query_table(*in.conn, in);
            if (matrix_tuple) {
                captured_query_rows captured =
                    query_rows_capturing_error(*in.conn, align_sql);
                if (captured.status != captured_query_status::tuples_ok) {
                    if (captured.status
                        == captured_query_status::connection_failure) {
                        (void)resources.stop();
                        const auto recovery =
                            reconnect_and_prove_measured_query_connection_failure(
                                in,
                                measured_query_session_identity(
                                    out, /*persisted_tuple=*/false));
                        if (!recovery.has_value()) {
                            abort_unexpected_algorithm_matrix_query_result(
                                captured, out);
                        }
                        record_measured_query_connection_failure(
                            captured, in, *recovery, out);
                        return out;
                    }
                    if (!is_allowlisted_algorithm_matrix_sql_rejection(
                            captured, in, out, /*persisted_tuple=*/false)) {
                        abort_unexpected_algorithm_matrix_query_result(
                            captured, out);
                    }
                    (void)resources.stop();
                    record_allowlisted_algorithm_matrix_sql_rejection(
                        captured, in, out);
                    return out;
                }
                out.sql_result_status = captured.result_status;
                parse_bioseq_batch_rows(
                    captured.rows, in, out, transformed_target_origin);
            } else {
                parse_bioseq_batch_rows(
                    it::query_rows(*in.conn, align_sql), in, out,
                    transformed_target_origin);
            }
        }
        const auto end_ns = static_cast<std::uint64_t>(bioseq::instrumentation::clock::now_ns());
        attach_resource_metrics(out, start_ns, end_ns, resources.stop());
        capture_alignment_statement_stats(*in.conn, prior_alignment, out);
        capture_hash_major_build_stats(*in.conn, out);
        capture_index_audit(
            *in.conn, in.no_index_table, minimizer_index, out);
        capture_cold_no_index_absence(
            *in.conn, in, out, cold_no_index_audit_stage::measured);
        require_cold_no_index_lifecycle(out);
        (void)require_pipeline_execution_stats(
            algo, in, out,
            make_pipeline_stats_expectation(in, false, fair_workers),
            minimizer_index);
        if (out.tuple_accepted == 1) {
            reject_invalid_matrix_result(in, out);
        }
        log_measured_case(
            algo, pipeline_mode, minimap_threads, in, out, minimizer_index);

    } else if (algo == "bioseq_block_gpu_dp_gpu_seed_no_index") {
        // The target exists in PostgreSQL before timing, but no minimizer
        // sidecar/generation exists.  Restart after the untimed load, then time
        // only exact GPU transient target-index construction plus mapping.
        const bool recompute_async_persistence =
            param_string(params, "recompute_async_persistence", "off") == "on";
        if (recompute_async_persistence) {
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
            *in.conn, in, out, cold_no_index_audit_stage::post_restart);
        const std::string align_sql =
            batched_coord_sql(in.no_index_table, proj, 1);
        const alignment_snapshot_identity prior_alignment =
            capture_prior_alignment_identity(*in.conn);
        scoped_resource_monitor resources;
        const auto start_ns = static_cast<std::uint64_t>(bioseq::instrumentation::clock::now_ns());
        {
            auto _scope = bioseq::instrumentation::region(k_measured_region);
            load_e2e_query_table(*in.conn, in);
            parse_bioseq_batch_rows(
                it::query_rows(*in.conn, align_sql), in, out);
        }
        const auto end_ns = static_cast<std::uint64_t>(bioseq::instrumentation::clock::now_ns());
        attach_resource_metrics(out, start_ns, end_ns, resources.stop());
        capture_alignment_statement_stats(*in.conn, prior_alignment, out);
        capture_hash_major_build_stats(*in.conn, out);
        capture_index_audit(
            *in.conn, in.no_index_table, minimizer_index, out);
        capture_cold_no_index_absence(
            *in.conn, in, out, cold_no_index_audit_stage::measured);
        require_cold_no_index_lifecycle(out);
        (void)require_pipeline_execution_stats(
            algo, in, out,
            make_pipeline_stats_expectation(in, false, fair_workers),
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
        it::exec_sql(*in.conn, "SET bioseq.index.insert_maintenance.enabled = off;");
        it::exec_sql(*in.conn, "SET bioseq.index.sequence_read.enabled   = off;");
        const std::string load_sql = fmt::format(
            "SELECT bioseq.load('{}'::regclass, '{}')",
            in.no_index_table, in.target_fasta);

        scoped_resource_monitor resources;
        const auto start_ns = static_cast<std::uint64_t>(bioseq::instrumentation::clock::now_ns());
        {
            auto _scope = bioseq::instrumentation::region(k_measured_region); // TIMED: load only
            it::exec_sql(*in.conn, load_sql);
        }
        const auto end_ns = static_cast<std::uint64_t>(bioseq::instrumentation::clock::now_ns());
        attach_resource_metrics(out, start_ns, end_ns, resources.stop());
        out.found = true;
        out.match_count = 1;
        capture_index_audit(*in.conn, in.no_index_table, minimizer_index, out);
        log_measured_case(
            algo, pipeline_mode, minimap_threads, in, out, minimizer_index);

    } else if (algo == "bioseq_recompute_only") {
        const bool recompute_async_persistence =
            param_string(params, "recompute_async_persistence", "off") == "on";
        configure_bioseq_scheduler_session(*in.conn, pipeline_mode);
        configure_bioseq_pair_sort_session(*in.conn, params);
        it::exec_sqlf(*in.conn, "TRUNCATE {};", in.no_index_table);
        configure_bioseq_minimizer_policy(
            *in.conn, in.no_index_table, minimizer_index);
        it::exec_sql(*in.conn, "SET bioseq.index.insert_maintenance.enabled = off;");
        it::exec_sql(*in.conn, "SET bioseq.index.sequence_read.enabled   = off;");
        it::exec_sqlf(*in.conn,
            "SELECT bioseq.load('{}'::regclass, '{}')",
            in.no_index_table, in.target_fasta);

        it::connection_test_context read_conn;
        configure_bioseq_aligner_session(read_conn, in.kmer_size, in.window_size);
        configure_bioseq_scheduler_session(read_conn, pipeline_mode);
        configure_bioseq_pair_sort_session(read_conn, params);
        it::exec_sql(read_conn, "SET bioseq.index.insert_maintenance.enabled = off;");
        it::exec_sql(read_conn, "SET bioseq.index.sequence_read.enabled   = off;");
        it::exec_sqlf(read_conn, "SET bioseq.index.recompute_async_persistence.enabled = {};",
                      recompute_async_persistence ? "on" : "off");

        const std::string align_sql = fmt::format(
            "SELECT header, {} FROM {} WHERE mmsearch(seq, {}, "
            "aligner_options(index_read_requirement => 1)) ORDER BY header",
            proj, in.no_index_table, qlit);
        scoped_resource_monitor resources;
        const auto start_ns = static_cast<std::uint64_t>(bioseq::instrumentation::clock::now_ns());
        {
            auto _scope = bioseq::instrumentation::region(k_measured_region); // TIMED: recompute align only
            parse_bioseq_rows(it::query_rows(read_conn, align_sql), in, out);
            capture_hash_major_build_stats(read_conn, out);
        }
        const auto end_ns = static_cast<std::uint64_t>(bioseq::instrumentation::clock::now_ns());
        attach_resource_metrics(out, start_ns, end_ns, resources.stop());
        require_mapped(algo, in, out);
        if (recompute_async_persistence) {
            capture_index_audit_after_async_persist(
                *in.conn, in.no_index_table, minimizer_index, out);
            capture_hash_major_build_stats(*in.conn, out);
        } else {
            capture_index_audit(
                *in.conn, in.no_index_table, minimizer_index, out);
        }
        log_measured_case(
            algo, pipeline_mode, minimap_threads, in, out, minimizer_index);

    } else if (algo == "minimap2_no_index") {
        scoped_resource_monitor resources;
        const auto start_ns = static_cast<std::uint64_t>(bioseq::instrumentation::clock::now_ns());
        {
            auto _scope = bioseq::instrumentation::region(k_measured_region); // TIMED: build + map
            const minimap2_helper_result helper = run_minimap2_helper(
                minimap2_receipt_mode::no_index, minimap_threads,
                in.kmer_size, in.window_size, in.queries.size(),
                in.minimap2_index_parts, in.target_fasta_split,
                in.query_fasta, true);
            parse_minimap2_paf(helper.stdout_text, in, out);
            attach_minimap2_receipt(out, helper);
        }
        const auto end_ns = static_cast<std::uint64_t>(bioseq::instrumentation::clock::now_ns());
        attach_resource_metrics(out, start_ns, end_ns, resources.stop());
        require_minimap2_union_within_manual_envelope(out);
        reject_invalid_matrix_result(in, out);
        log_measured_case(algo, pipeline_mode, minimap_threads, in, out);

    } else if (algo == "minimap2_just_index") {
        if (in.paired_minimap2_oracle) {
            const auto chunks = make_logical_query_chunks(
                in.queries.size(), in.logical_query_chunk_size);
            if (chunks.size() != in.query_chunk_fastas.size()) {
                throw std::runtime_error(
                    "logical minimap2 query FASTAs do not match the chunk plan");
            }
            std::vector<logical_minimap2_chunk_result> results;
            results.reserve(chunks.size());
            std::string logical_paf;
            std::uint64_t measured_sum = 0;
            std::uint64_t validation_sum = 0;
            for (std::size_t ordinal = 0; ordinal < chunks.size(); ++ordinal) {
                const logical_query_chunk chunk = chunks[ordinal];
                scoped_resource_monitor resources;
                const auto start_ns = static_cast<std::uint64_t>(
                    bioseq::instrumentation::clock::now_ns());
                std::optional<pending_minimap2_helper_result> pending;
                std::string failure;
                try {
                    auto _scope = bioseq::instrumentation::region(
                        k_measured_region);
                    pending.emplace(execute_minimap2_helper(
                        minimap2_receipt_mode::prebuilt_index,
                        minimap_threads, in.kmer_size, in.window_size,
                        chunk.size(), in.minimap2_index_parts, in.mmi_path,
                        in.query_chunk_fastas[ordinal], true,
                        std::chrono::milliseconds{
                            static_cast<std::chrono::milliseconds::rep>(
                                in.statement_timeout_ms)}));
                } catch (const std::exception& error) {
                    failure = error.what();
                }
                const auto end_ns = static_cast<std::uint64_t>(
                    bioseq::instrumentation::clock::now_ns());
                e2e_output chunk_resources;
                attach_resource_metrics(
                    chunk_resources, start_ns, end_ns, resources.stop());
                measured_sum = checked_logical_u64_sum(
                    measured_sum, chunk_resources.measured_region_ns,
                    "minimap2 measured algorithm");
                out.peak_postgres_rss_kb = std::max(
                    out.peak_postgres_rss_kb,
                    chunk_resources.peak_postgres_rss_kb);
                out.peak_minimap2_rss_kb = std::max(
                    out.peak_minimap2_rss_kb,
                    chunk_resources.peak_minimap2_rss_kb);
                out.peak_bench_rss_kb = std::max(
                    out.peak_bench_rss_kb,
                    chunk_resources.peak_bench_rss_kb);
                if (out.min_tmp_available_kb == 0) {
                    out.min_tmp_available_kb =
                        chunk_resources.min_tmp_available_kb;
                } else if (chunk_resources.min_tmp_available_kb != 0) {
                    out.min_tmp_available_kb = std::min(
                        out.min_tmp_available_kb,
                        chunk_resources.min_tmp_available_kb);
                }
                out.peak_swap_used_mb = std::max(
                    out.peak_swap_used_mb,
                    chunk_resources.peak_swap_used_mb);
                const auto validation_begin_ns = static_cast<std::uint64_t>(
                    bioseq::instrumentation::clock::now_ns());
                std::optional<minimap2_helper_result> helper;
                if (failure.empty()) {
                    try {
                        helper.emplace(finalize_minimap2_helper(
                            std::move(*pending)));
                        if (helper->receipt.combined_union_ns
                            > chunk_resources.measured_region_ns) {
                            throw std::runtime_error(
                                "physical minimap2 interval union exceeds "
                                "its measured helper envelope");
                        }
                    } catch (const std::exception& error) {
                        failure = error.what();
                    }
                }
                if (failure.empty()) {
                    logical_paf.append(helper->stdout_text);
                    results.push_back(logical_minimap2_chunk_result{
                        .query_begin = chunk.begin,
                        .query_end = chunk.end,
                        .measured_ns = chunk_resources.measured_region_ns,
                        .helper = std::move(*helper),
                    });
                }
                const auto validation_end_ns = static_cast<std::uint64_t>(
                    bioseq::instrumentation::clock::now_ns());
                if (validation_end_ns >= validation_begin_ns) {
                    validation_sum = checked_logical_u64_sum(
                        validation_sum,
                        validation_end_ns - validation_begin_ns,
                        "minimap2 excluded validation");
                }
                if (!failure.empty()) {
                    out.measured_region_ns = measured_sum;
                    out.measured_region_available = true;
                    out.validation_excluded_ns = validation_sum;
                    out.logical_chunking_enabled = 1;
                    out.logical_chunk_count = static_cast<std::int64_t>(
                        ordinal + 1u);
                    out.logical_chunk_size = static_cast<std::int64_t>(
                        in.logical_query_chunk_size);
                    out.logical_statement_timeout_ms =
                        static_cast<std::int64_t>(in.statement_timeout_ms);
                    out.logical_statement_timeout_restored = 1;
                    record_logical_chunk_failure(
                        out, algo, chunk, failure);
                    nlohmann::json completed = nlohmann::json::array();
                    for (std::size_t completed_ordinal = 0;
                         completed_ordinal < results.size();
                         ++completed_ordinal) {
                        const auto& result = results[completed_ordinal];
                        completed.push_back(nlohmann::json{
                            {"ordinal", completed_ordinal},
                            {"query_begin", result.query_begin},
                            {"query_end", result.query_end},
                            {"receipt_checksum",
                                result.helper.receipt.checksum},
                            {"receipt_artifact_path",
                                result.helper.artifact_path},
                            {"receipt_artifact_digest",
                                result.helper.artifact_digest},
                        });
                    }
                    out.logical_chunk_receipts = nlohmann::json{
                        {"schema",
                            "bioseq.e2e.logical-minimap2-chunk-receipt"},
                        {"version", 1},
                        {"complete", false},
                        {"failed_ordinal", ordinal},
                        {"physical_chunks", std::move(completed)},
                    };
                    break;
                }
            }
            if (out.tuple_accepted == 1) {
                attach_logical_minimap2_receipt(in, out, results);
                out.validation_excluded_ns = validation_sum;
                const auto parse_begin_ns = static_cast<std::uint64_t>(
                    bioseq::instrumentation::clock::now_ns());
                parse_minimap2_paf(logical_paf, in, out);
                reject_invalid_matrix_result(in, out);
                const auto parse_end_ns = static_cast<std::uint64_t>(
                    bioseq::instrumentation::clock::now_ns());
                if (parse_end_ns >= parse_begin_ns) {
                    out.validation_excluded_ns = checked_logical_u64_sum(
                        out.validation_excluded_ns,
                        parse_end_ns - parse_begin_ns,
                        "minimap2 excluded row validation");
                }
                require_minimap2_union_within_manual_envelope(out);
            }
            log_measured_case(
                algo, pipeline_mode, minimap_threads, in, out);
        } else {
            scoped_resource_monitor resources;
            const auto start_ns = static_cast<std::uint64_t>(bioseq::instrumentation::clock::now_ns());
            {
                auto _scope = bioseq::instrumentation::region(k_measured_region); // TIMED: map only
                const minimap2_helper_result helper = run_minimap2_helper(
                    minimap2_receipt_mode::prebuilt_index, minimap_threads,
                    in.kmer_size, in.window_size, in.queries.size(),
                    in.minimap2_index_parts, in.mmi_path, in.query_fasta, true);
                parse_minimap2_paf(helper.stdout_text, in, out);
                attach_minimap2_receipt(out, helper);
            }
            const auto end_ns = static_cast<std::uint64_t>(bioseq::instrumentation::clock::now_ns());
            attach_resource_metrics(out, start_ns, end_ns, resources.stop());
            require_minimap2_union_within_manual_envelope(out);
            reject_invalid_matrix_result(in, out);
            log_measured_case(algo, pipeline_mode, minimap_threads, in, out);
        }

    } else {
        bioseq_log<logging_level::error>("[bench_e2e] unknown algorithm '{}'", algo);
    }
    finalize_e2e_priority_attestation(algo, in, out);
    annotate_accepted_behavior_differences(in, out);
    return out;
}

/** @brief Identify duration-valued evidence fields in framework/raw output. */
[[nodiscard]] bool is_e2e_numeric_timing_key(
        std::string_view key) noexcept {
    return key.ends_with("_ns") || key.ends_with("_ns_manual");
}

/** @brief Null non-finite values and numeric timings forbidden on rejected rows. */
void sanitize_e2e_raw_numbers(
        nlohmann::json& value,
        const bool redact_numeric_timings) {
    if (value.is_object()) {
        for (auto& [key, child] : value.items()) {
            if (redact_numeric_timings
                && is_e2e_numeric_timing_key(key)
                && child.is_number()) {
                child = nullptr;
                continue;
            }
            sanitize_e2e_raw_numbers(child, redact_numeric_timings);
        }
        return;
    }
    if (value.is_array()) {
        for (auto& child : value) {
            sanitize_e2e_raw_numbers(child, redact_numeric_timings);
        }
        return;
    }
    if (value.is_number_float()
        && !std::isfinite(value.get<double>())) {
        value = nullptr;
    }
}

// ---- §10.4 on_evaluate ----
[[nodiscard]] std::map<std::string, double> e2e_eval(
    std::string_view, const parameter_map&, const e2e_input&, const e2e_output& o) {
    const bool accepted_timing_available =
        o.tuple_accepted == 1 && o.measured_region_available;
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
        {"mapq_result_count",       static_cast<double>(o.mapq_result_count)},
        {"ordered_query_oracle_available",
            static_cast<double>(o.ordered_query_oracle_available)},
        {"ordered_query_oracle_sha256_word_0",
            static_cast<double>(o.ordered_query_oracle_sha256_words[0])},
        {"ordered_query_oracle_sha256_word_1",
            static_cast<double>(o.ordered_query_oracle_sha256_words[1])},
        {"ordered_query_oracle_sha256_word_2",
            static_cast<double>(o.ordered_query_oracle_sha256_words[2])},
        {"ordered_query_oracle_sha256_word_3",
            static_cast<double>(o.ordered_query_oracle_sha256_words[3])},
        {"ordered_query_oracle_sha256_word_4",
            static_cast<double>(o.ordered_query_oracle_sha256_words[4])},
        {"ordered_query_oracle_sha256_word_5",
            static_cast<double>(o.ordered_query_oracle_sha256_words[5])},
        {"ordered_query_oracle_sha256_word_6",
            static_cast<double>(o.ordered_query_oracle_sha256_words[6])},
        {"ordered_query_oracle_sha256_word_7",
            static_cast<double>(o.ordered_query_oracle_sha256_words[7])},
        {"paired_oracle_comparison_available",
            static_cast<double>(o.paired_oracle_comparison_available)},
        {"paired_oracle_exact", static_cast<double>(o.paired_oracle_exact)},
        {"paired_oracle_verdict_sealed",
            static_cast<double>(o.paired_oracle_verdict_sealed)},
        {"paired_mapq_vector_equal",
            static_cast<double>(o.paired_mapq_vector_equal)},
        {"logical_chunking_enabled",
            static_cast<double>(o.logical_chunking_enabled)},
        {"logical_chunk_count",
            static_cast<double>(o.logical_chunk_count)},
        {"logical_chunk_size",
            static_cast<double>(o.logical_chunk_size)},
        {"logical_statement_timeout_ms",
            static_cast<double>(o.logical_statement_timeout_ms)},
        {"logical_statement_timeout_restored",
            static_cast<double>(o.logical_statement_timeout_restored)},
        {"priority_target_nice", static_cast<double>(o.priority_target_nice)},
        {"runner_nice", static_cast<double>(o.runner_nice)},
        {"postgres_backend_nice",
            static_cast<double>(o.postgres_backend_nice)},
        {"priority_attestation_accepted",
            static_cast<double>(o.priority_attestation_accepted)},
        {"tuple_accepted",          static_cast<double>(o.tuple_accepted)},
        {"prefilter_execution_stats_available",
            static_cast<double>(o.prefilter_execution_stats_available)},
        {"prefilter_activity_count",
            static_cast<double>(o.prefilter_activity_count)},
        {"prefilter_execution_stats_saturated",
            static_cast<double>(o.prefilter_execution_stats_saturated)},
        {"found_fraction",          o.found ? 1.0 : 0.0},
        {"pipeline_validation_present", static_cast<double>(o.pipeline_stats_available)},
        {"pipeline_validation_attached_to_measurement", static_cast<double>(o.pipeline_stats_attached_to_measurement)},
        {"pipeline_validation_accepted", static_cast<double>(o.pipeline_stats_consistent)},
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
        {"simd_stats_schema_valid", static_cast<double>(o.simd_stats_schema_valid)},
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
        {"block_aligner_execution_stats_available", static_cast<double>(o.block_aligner_execution_stats_available)},
        {"block_aligner_execution_stats_consistent", static_cast<double>(o.block_aligner_execution_stats_consistent)},
        {"block_aligner_executed_calls", static_cast<double>(o.block_aligner_executed_calls)},
        {"block_aligner_nonempty_calls", static_cast<double>(o.block_aligner_nonempty_calls)},
        {"block_aligner_nonempty_calls_with_simd", static_cast<double>(o.block_aligner_nonempty_calls_with_simd)},
        {"block_aligner_simd_dp_vector_batches", static_cast<double>(o.block_aligner_simd_dp_vector_batches)},
        {"block_aligner_simd_dp_cells", static_cast<double>(o.block_aligner_simd_dp_cells)},
        {"block_aligner_forbidden_ksw2_calls", static_cast<double>(o.block_aligner_forbidden_ksw2_calls)},
        {"block_aligner_forbidden_scalar_biological_fallbacks", static_cast<double>(o.block_aligner_forbidden_scalar_biological_fallbacks)},
        {"block_aligner_execution_stats_saturated", static_cast<double>(o.block_aligner_execution_stats_saturated)},
        {"nthash_vector_batches", static_cast<double>(o.nthash_vector_batches)},
        {"nthash_resident_vector_stream_kernel_calls", static_cast<double>(o.nthash_resident_vector_stream_kernel_calls)},
        {"simd_chaining_exact_policy_calls", static_cast<double>(o.simd_chaining_exact_policy_calls)},
        {"simd_chaining_bounded_policy_calls", static_cast<double>(o.simd_chaining_bounded_policy_calls)},
        {"simd_chaining_bounded_score_batches", static_cast<double>(o.simd_chaining_bounded_score_batches)},
        {"simd_chaining_bounded_cutoff_stops", static_cast<double>(o.simd_chaining_bounded_cutoff_stops)},
        {"simd_chaining_bounded_candidates_skipped", static_cast<double>(o.simd_chaining_bounded_candidates_skipped)},
        {"simd_chaining_policy_rejections", static_cast<double>(o.simd_chaining_policy_rejections)},
        {"simd_chaining_policy_stats_saturated", static_cast<double>(o.simd_chaining_policy_stats_saturated)},
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
        {"grouped_soa_reader_capacity_accounting_passes", static_cast<double>(o.grouped_soa_reader_capacity_accounting_passes)},
        {"grouped_soa_reader_global_position_storage_preparations", static_cast<double>(o.grouped_soa_reader_global_position_storage_preparations)},
        {"grouped_soa_reader_global_position_storage_allocations", static_cast<double>(o.grouped_soa_reader_global_position_storage_allocations)},
        {"grouped_soa_reader_position_append_growth_reservations", static_cast<double>(o.grouped_soa_reader_position_append_growth_reservations)},
        {"grouped_soa_reader_retained_position_count", static_cast<double>(o.grouped_soa_reader_retained_position_count)},
        {"grouped_soa_reader_retained_position_capacity", static_cast<double>(o.grouped_soa_reader_retained_position_capacity)},
        {"grouped_soa_reader_directory_capacity_bytes", static_cast<double>(o.grouped_soa_reader_directory_capacity_bytes)},
        {"grouped_soa_reader_query_hash_capacity_bytes", static_cast<double>(o.grouped_soa_reader_query_hash_capacity_bytes)},
        {"grouped_soa_reader_query_record_capacity_bytes", static_cast<double>(o.grouped_soa_reader_query_record_capacity_bytes)},
        {"grouped_soa_reader_ordinal_route_capacity_bytes", static_cast<double>(o.grouped_soa_reader_ordinal_route_capacity_bytes)},
        {"grouped_soa_reader_selected_extent_capacity_bytes", static_cast<double>(o.grouped_soa_reader_selected_extent_capacity_bytes)},
        {"grouped_soa_reader_current_selected_control_capacity_bytes", static_cast<double>(o.grouped_soa_reader_current_selected_control_capacity_bytes)},
        {"grouped_soa_reader_global_position_capacity_bytes", static_cast<double>(o.grouped_soa_reader_global_position_capacity_bytes)},
        {"grouped_soa_reader_control_payload_capacity_bytes", static_cast<double>(o.grouped_soa_reader_control_payload_capacity_bytes)},
        {"grouped_soa_reader_owned_capacity_bytes", static_cast<double>(o.grouped_soa_reader_owned_capacity_bytes)},
        {"grouped_soa_reader_admitted_bytes", static_cast<double>(o.grouped_soa_reader_admitted_bytes)},
        {"grouped_soa_reader_exact_position_image_checks", static_cast<double>(o.grouped_soa_reader_exact_position_image_checks)},
        {"grouped_soa_reader_exact_position_image_failures", static_cast<double>(o.grouped_soa_reader_exact_position_image_failures)},
        {"grouped_soa_reader_retained_layout_checks", static_cast<double>(o.grouped_soa_reader_retained_layout_checks)},
        {"grouped_soa_reader_retained_layout_failures", static_cast<double>(o.grouped_soa_reader_retained_layout_failures)},
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
        {"fragment_native_peak_block_stats_capacity_bytes", static_cast<double>(o.fragment_native_peak_block_stats_capacity_bytes)},
        {"fragment_native_occurrence_allocations", static_cast<double>(o.fragment_native_occurrence_allocations)},
        {"fragment_native_query_plan_allocations", static_cast<double>(o.fragment_native_query_plan_allocations)},
        {"fragment_native_query_group_offset_allocations", static_cast<double>(o.fragment_native_query_group_offset_allocations)},
        {"fragment_native_query_membership_filter_allocations", static_cast<double>(o.fragment_native_query_membership_filter_allocations)},
        {"fragment_native_query_radix_allocations", static_cast<double>(o.fragment_native_query_radix_allocations)},
        {"fragment_native_occurrence_count_allocations", static_cast<double>(o.fragment_native_occurrence_count_allocations)},
        {"fragment_native_block_layout_allocations", static_cast<double>(o.fragment_native_block_layout_allocations)},
        {"fragment_native_block_stats_allocations", static_cast<double>(o.fragment_native_block_stats_allocations)},
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
        {"are_fragment_native_stats_saturated", static_cast<double>(o.are_fragment_native_stats_saturated)},
        {"are_grouped_soa_reader_memory_stats_saturated", static_cast<double>(o.are_grouped_soa_reader_memory_stats_saturated)},
        {"persisted_grouped_soa_layout", static_cast<double>(
            o.persisted_layout == "grouped_soa")},
        {"persisted_hash_major_layout", static_cast<double>(
            o.persisted_layout == "hash_major")},
        {"persisted_format6_contract_valid", static_cast<double>(o.persisted_format6_contract_valid)},
        {"format6_restart_audit_accepted", static_cast<double>(o.format6_restart_audit_accepted)},
        {"hash_major_directory_rows", static_cast<double>(o.hash_major_directory_rows)},
        {"hash_major_directory_bytes", static_cast<double>(o.hash_major_directory_bytes)},
        {"hash_major_control_rows", static_cast<double>(o.hash_major_control_rows)},
        {"hash_major_control_bytes", static_cast<double>(o.hash_major_control_bytes)},
        {"hash_major_posting_rows", static_cast<double>(o.hash_major_posting_rows)},
        {"hash_major_posting_bytes", static_cast<double>(o.hash_major_posting_bytes)},
        {"hash_major_posting_ranges_delivered", static_cast<double>(o.hash_major_posting_ranges_delivered)},
        {"hash_major_selected_block_count", static_cast<double>(o.hash_major_selected_block_count)},
        {"hash_major_accepted_hash_count", static_cast<double>(o.hash_major_accepted_hash_count)},
        {"hash_major_retained_position_count", static_cast<double>(o.hash_major_retained_position_count)},
        {"hash_major_authorized_posting_bytes", static_cast<double>(o.hash_major_authorized_posting_bytes)},
        {"hash_major_peak_admitted_bytes", static_cast<double>(o.hash_major_peak_admitted_bytes)},
        {"hash_major_retained_owner_bytes", static_cast<double>(o.hash_major_retained_owner_bytes)},
        {"hash_major_admitted_bytes", static_cast<double>(o.hash_major_admitted_bytes)},
        {"hash_major_failures", static_cast<double>(o.hash_major_failures)},
        {"hash_major_is_saturated", static_cast<double>(o.hash_major_is_saturated)},
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
        {"cold_no_index_pre_restart_minimizer_fragment_rows", static_cast<double>(o.cold_no_index_pre_restart_minimizer_fragment_rows)},
        {"cold_no_index_pre_restart_minimizer_sequence_rows", static_cast<double>(o.cold_no_index_pre_restart_minimizer_sequence_rows)},
        {"cold_no_index_post_restart_minimizer_fragment_rows", static_cast<double>(o.cold_no_index_post_restart_minimizer_fragment_rows)},
        {"cold_no_index_post_restart_minimizer_sequence_rows", static_cast<double>(o.cold_no_index_post_restart_minimizer_sequence_rows)},
        {"cold_no_index_pre_restart_async_jobs", static_cast<double>(o.cold_no_index_pre_restart_async_jobs)},
        {"cold_no_index_post_restart_async_jobs", static_cast<double>(o.cold_no_index_post_restart_async_jobs)},
        {"cold_no_index_pre_restart_gucs_off", static_cast<double>(o.cold_no_index_pre_restart_gucs_off)},
        {"cold_no_index_post_restart_gucs_off", static_cast<double>(o.cold_no_index_post_restart_gucs_off)},
        {"cold_no_index_lifecycle_accepted", static_cast<double>(o.cold_no_index_lifecycle_accepted)},
        {"persisted_policy_audit_accepted",
            static_cast<double>(o.persisted_policy_audit_accepted)},
        {"audit_selected_minimizer_is_sequence", static_cast<double>(o.audit_selected_minimizer_is_sequence)},
        {"audit_minimizer_fragment_rows",          static_cast<double>(o.audit_minimizer_fragment_rows)},
        {"audit_minimizer_sequence_rows",          static_cast<double>(o.audit_minimizer_sequence_rows)},
        {"audit_minimizer_fragment_update_on_insert", static_cast<double>(o.audit_minimizer_fragment_update_on_insert)},
        {"audit_minimizer_sequence_update_on_insert", static_cast<double>(o.audit_minimizer_sequence_update_on_insert)},
        {"audit_minimizer_fragment_active_read",   static_cast<double>(o.audit_minimizer_fragment_active_read)},
        {"audit_minimizer_sequence_active_read",   static_cast<double>(o.audit_minimizer_sequence_active_read)},
        {"audit_async_persist_jobs_waiting", static_cast<double>(o.audit_async_persist_jobs_waiting)},
        {"audit_index_rows_wait_attempts", static_cast<double>(o.audit_index_rows_wait_attempts)},
        {"hash_major_build_backend_pid", static_cast<double>(o.hash_major_build_backend_pid)},
        {"hash_major_build_db_oid", static_cast<double>(o.hash_major_build_db_oid)},
        {"hash_major_build_relfilenode", static_cast<double>(o.hash_major_build_relfilenode)},
        {"hash_major_build_sequence_rowno", static_cast<double>(o.hash_major_build_sequence_rowno)},
        {"hash_major_build_total_run_count", static_cast<double>(o.hash_major_build_total_run_count)},
        {"hash_major_build_resident_runs", static_cast<double>(o.hash_major_build_resident_runs)},
        {"hash_major_build_file_runs", static_cast<double>(o.hash_major_build_file_runs)},
        {"hash_major_build_resident_run_bytes", static_cast<double>(o.hash_major_build_resident_run_bytes)},
        {"hash_major_build_resident_peak_bytes", static_cast<double>(o.hash_major_build_resident_peak_bytes)},
        {"hash_major_build_spilled_run_bytes", static_cast<double>(o.hash_major_build_spilled_run_bytes)},
        {"hash_major_build_postgres_temp_file_bytes", static_cast<double>(o.hash_major_build_postgres_temp_file_bytes)},
        {"hash_major_build_postgres_temp_file_count", static_cast<double>(o.hash_major_build_postgres_temp_file_count)},
        {"hash_major_build_merge_reader_buffer_bytes", static_cast<double>(o.hash_major_build_merge_reader_buffer_bytes)},
        {"hash_major_build_merge_pass_count", static_cast<double>(o.hash_major_build_merge_pass_count)},
        {"hash_major_build_spill_count_from_pressure", static_cast<double>(o.hash_major_build_spill_count_from_pressure)},
        {"hash_major_build_resident_demotions", static_cast<double>(o.hash_major_build_resident_demotions)},
        {"hash_major_build_position_spill_bytes", static_cast<double>(o.hash_major_build_position_spill_bytes)},
        {"hash_major_build_position_spill_count", static_cast<double>(o.hash_major_build_position_spill_count)},
        {"hash_major_build_shared_generation", static_cast<double>(o.hash_major_build_shared_generation)},
        {"hash_major_build_worker_build_ns", static_cast<double>(o.hash_major_build_worker_build_ns)},
        {"hash_major_build_pipeline_mode", static_cast<double>(o.hash_major_build_pipeline_mode)},
        {"hash_major_build_max_concurrency", static_cast<double>(o.hash_major_build_max_concurrency)},
        {"hash_major_build_index_memory_cap_mb", static_cast<double>(o.hash_major_build_index_memory_cap_mb)},
        {"hash_major_build_lazy_worker_shareable", static_cast<double>(o.hash_major_build_lazy_worker_shareable)},
    };
    if (o.position_offset_available
        && std::isfinite(o.abs_offset_bp)
        && std::isfinite(o.max_abs_offset_bp)) {
        metrics.emplace("mean_position_offset_bp", o.abs_offset_bp);
        metrics.emplace("max_position_offset_bp", o.max_abs_offset_bp);
    }
    if (accepted_timing_available) {
        metrics.emplace("measured_region_ns_manual",
                        static_cast<double>(o.measured_region_ns));
    }
    if (o.downstream_active_wall_available >= 0) {
        metrics.emplace("downstream_active_wall_available",
                        static_cast<double>(o.downstream_active_wall_available));
    }
    if (accepted_timing_available
        && o.downstream_active_wall_available == 1) {
        metrics.emplace("downstream_active_wall_ns",
                        static_cast<double>(o.downstream_active_wall_ns));
    }
    if (o.downstream_active_wall_invocation_count >= 0) {
        metrics.emplace("downstream_active_wall_invocation_count",
                        static_cast<double>(o.downstream_active_wall_invocation_count));
    }
    if (o.downstream_active_wall_unavailable_invocation_count >= 0) {
        metrics.emplace("downstream_active_wall_unavailable_invocation_count",
                        static_cast<double>(
                            o.downstream_active_wall_unavailable_invocation_count));
    }
    if (accepted_timing_available && o.downstream_work_ns >= 0) {
        metrics.emplace("downstream_work_ns",
                        static_cast<double>(o.downstream_work_ns));
    }
    if (o.downstream_work_saturated >= 0) {
        metrics.emplace("downstream_work_saturated",
                        static_cast<double>(o.downstream_work_saturated));
    }
    if (o.mapq_execution_available >= 0) {
        metrics.emplace("mapq_execution_available",
                        static_cast<double>(o.mapq_execution_available));
        metrics.emplace("mapq_executed", static_cast<double>(o.mapq_executed));
        metrics.emplace("mapq_execution_conflict",
                        static_cast<double>(o.mapq_execution_conflict));
    }
    if (o.benchmark_e2e_interval_schema_revision >= 0) {
        metrics.emplace("benchmark_e2e_interval_schema_revision",
                        static_cast<double>(
                            o.benchmark_e2e_interval_schema_revision));
    }
    if (o.benchmark_e2e_interval_available >= 0) {
        metrics.emplace("benchmark_e2e_interval_available",
                        static_cast<double>(o.benchmark_e2e_interval_available));
        metrics.emplace("benchmark_e2e_interval_count",
                        static_cast<double>(o.benchmark_e2e_interval_count));
        metrics.emplace("benchmark_e2e_cigar_interval_count",
                        static_cast<double>(o.benchmark_e2e_cigar_interval_count));
        metrics.emplace("benchmark_e2e_mapq_interval_count",
                        static_cast<double>(o.benchmark_e2e_mapq_interval_count));
        metrics.emplace("benchmark_e2e_interval_finalization_count",
                        static_cast<double>(
                            o.benchmark_e2e_interval_finalization_count));
    }
    if (o.minimap2_receipt_available >= 0) {
        metrics.emplace("minimap2_receipt_available",
                        static_cast<double>(o.minimap2_receipt_available));
        metrics.emplace("minimap2_cigar_interval_count",
                        static_cast<double>(o.minimap2_cigar_interval_count));
        metrics.emplace("minimap2_mapq_interval_count",
                        static_cast<double>(o.minimap2_mapq_interval_count));
    }
    const std::int64_t cigar_or_mapq_union_ns =
        o.benchmark_e2e_interval_available == 1
            ? o.benchmark_e2e_interval_union_ns
            : o.minimap2_receipt_available == 1
                ? o.minimap2_cigar_or_mapq_union_ns : -1;
    if (accepted_timing_available && cigar_or_mapq_union_ns >= 0) {
        metrics.emplace("cigar_or_mapq_interval_union_ns",
                        static_cast<double>(cigar_or_mapq_union_ns));
    }
    const bool minimizer_fragment_reader_stats_valid = json_has_exact_keys(
            o.minimizer_fragment_reader_stats,
            k_minimizer_fragment_reader_stats_keys)
        && o.minimizer_fragment_reader_stats["schema"].is_string()
        && o.minimizer_fragment_reader_stats["schema"]
            == "bioseq.grouped_soa_reader_memory_stats"
        && o.minimizer_fragment_reader_stats["revision"].is_number_integer()
        && o.minimizer_fragment_reader_stats["revision"] == 6;
    for (const std::string_view key
         : k_grouped_soa_provider_stats_counter_keys) {
        const double value = minimizer_fragment_reader_stats_valid
            ? static_cast<double>(required_json_counter(
                o.minimizer_fragment_reader_stats, key))
            : -1.0;
        metrics.emplace(std::string{key}, value);
    }
    for (auto metric = metrics.begin(); metric != metrics.end();) {
        const bool rejected_timing = o.tuple_accepted != 1
            && is_e2e_numeric_timing_key(metric->first);
        if (rejected_timing || !std::isfinite(metric->second)) {
            metric = metrics.erase(metric);
        } else {
            ++metric;
        }
    }
    return metrics;
}

[[noreturn]] void abort_raw_stats_serialization(
        std::string_view reason) {
    std::fprintf(
        stderr,
        "[bench_e2e] RAW-STATS-SERIALIZATION-FAIL %.*s\n",
        static_cast<int>(reason.size()),
        reason.data());
    std::fflush(stderr);
    std::abort();
}

[[nodiscard]] nlohmann::json serialize_alignment_snapshot_rows(
        const e2e_output& out,
        const bool include_numeric_timings) {
    if (out.alignment_snapshot_rows.size()
        != e2e_output::k_alignment_snapshot_row_count) {
        abort_raw_stats_serialization(
            "PostgreSQL sample is missing the exact 20-row alignment SRF");
    }

    nlohmann::json snapshot = nlohmann::json::object();
    snapshot["schema"] =
        "bioseq.last_alignment_execution_stats/raw-rowset";
    snapshot["schema_revision"] = 2;
    snapshot["expected_row_count"] =
        e2e_output::k_alignment_snapshot_row_count;
    snapshot["actual_row_count"] = out.alignment_snapshot_rows.size();
    snapshot["expected_column_count"] =
        e2e_output::k_alignment_snapshot_column_count;

    nlohmann::json columns = nlohmann::json::array();
    for (std::size_t column = 0;
         column < k_alignment_snapshot_column_names.size(); ++column) {
        const std::string_view json_type = alignment_stats_json_column(column)
            ? std::string_view{"object"}
            : alignment_stats_text_column(column)
                ? std::string_view{"string"}
                : alignment_stats_boolean_column(column)
                    ? std::string_view{"boolean"}
                    : std::string_view{"integer"};
        columns.push_back(nlohmann::json{
            {"ordinal", column},
            {"name", k_alignment_snapshot_column_names[column]},
            {"json_type", json_type},
        });
    }
    snapshot["columns"] = std::move(columns);

    nlohmann::json serialized_rows = nlohmann::json::array();
    for (const auto& row : out.alignment_snapshot_rows) {
        if (row.size() != e2e_output::k_alignment_snapshot_column_count) {
            abort_raw_stats_serialization(
				"alignment SRF row does not have exactly 151 columns");
        }
        nlohmann::json serialized_row = nlohmann::json::array();
        for (std::size_t column = 0; column < row.size(); ++column) {
            if (!include_numeric_timings
                && is_e2e_numeric_timing_key(
                    k_alignment_snapshot_column_names[column])) {
                serialized_row.push_back(nullptr);
                continue;
            }
            const auto& cell = row[column];
            if (!cell.has_value()) {
                serialized_row.push_back(nullptr);
                continue;
            }
            if (alignment_stats_json_column(column)) {
                try {
                    serialized_row.push_back(nlohmann::json::parse(*cell));
                } catch (const nlohmann::json::exception&) {
                    abort_raw_stats_serialization(
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
                    abort_raw_stats_serialization(
                        "alignment SRF Boolean changed after validation");
                }
                serialized_row.push_back(*parsed != 0);
                continue;
            }
            const auto parsed = parse_snapshot_i64(cell);
            if (!parsed.has_value()) {
                abort_raw_stats_serialization(
                    "alignment SRF integer changed after validation");
            }
            serialized_row.push_back(*parsed);
        }
        serialized_rows.push_back(std::move(serialized_row));
    }
    snapshot["rows"] = std::move(serialized_rows);
    return snapshot;
}

[[nodiscard]] bool exact_logical_chunk_receipt_valid(
        const e2e_input& in,
        const e2e_output& out,
        const bool postgres_sample,
        const bool minimap2_sample) {
    if (!in.paired_minimap2_oracle
        || postgres_sample == minimap2_sample
        || out.logical_chunking_enabled != 1
        || out.logical_statement_timeout_restored != 1
        || out.logical_chunk_size
            != static_cast<std::int64_t>(in.logical_query_chunk_size)
        || out.logical_statement_timeout_ms
            != static_cast<std::int64_t>(in.statement_timeout_ms)
        || !out.logical_chunk_receipts.is_object()) {
        return false;
    }
    const auto expected = make_logical_query_chunks(
        in.queries.size(), in.logical_query_chunk_size);
    const std::string_view schema = postgres_sample
        ? "bioseq.e2e.logical-postgres-chunk-receipt"
        : "bioseq.e2e.logical-minimap2-chunk-receipt";
    const auto& receipt = out.logical_chunk_receipts;
    if (receipt.value("schema", std::string{}) != schema
        || receipt.value("version", 0) != 1
        || receipt.value("query_count", std::size_t{}) != in.queries.size()
        || receipt.value("chunk_size", std::size_t{})
            != in.logical_query_chunk_size
        || receipt.value("chunk_count", std::size_t{}) != expected.size()
        || receipt.value("statement_timeout_ms", std::uint64_t{})
            != in.statement_timeout_ms
        || !receipt.contains("physical_chunks")
        || !receipt["physical_chunks"].is_array()
        || receipt["physical_chunks"].size() != expected.size()
        || out.logical_chunk_count
            != static_cast<std::int64_t>(expected.size())) {
        return false;
    }
    if (minimap2_sample) {
        if (!receipt.contains("setup_receipt")
            || !receipt["setup_receipt"].is_object()) {
            return false;
        }
        const auto& setup = receipt["setup_receipt"];
        if (setup.value("checksum", std::string{})
                != in.minimap2_setup_receipt_checksum
            || setup.value("artifact_path", std::string{})
                != in.minimap2_setup_receipt_artifact_path
            || setup.value("artifact_digest", std::string{})
                != in.minimap2_setup_receipt_artifact_digest
            || !setup.contains("validated_summary")
            || setup["validated_summary"]
                != in.minimap2_setup_receipt_summary) {
            return false;
        }
    } else if (receipt.value("cold_restart_count", 0) != 1
        || receipt.value("shared_postmaster_start_time", std::string{})
            != out.timed_postmaster_start_time
        || receipt.value("shared_postgres_backend_pid", std::int64_t{-1})
            != out.alignment_backend_pid
        || receipt.value("shared_database_oid", std::int64_t{-1})
            != out.alignment_database_oid
        || receipt.value("shared_persisted_generation_id", std::int64_t{-1})
            != out.persisted_generation_id) {
        return false;
    }
    __int128 measured_sum = 0;
    __int128 union_sum = 0;
    for (std::size_t ordinal = 0; ordinal < expected.size(); ++ordinal) {
        const auto& physical = receipt["physical_chunks"][ordinal];
        if (!physical.is_object()
            || physical.value("ordinal", std::size_t{}) != ordinal
            || physical.value("query_begin", std::size_t{})
                != expected[ordinal].begin
            || physical.value("query_end", std::size_t{})
                != expected[ordinal].end
            || physical.value("query_count", std::size_t{})
                != expected[ordinal].size()
            || !physical.contains("measured_algorithm_ns")
            || !(physical["measured_algorithm_ns"].is_number_unsigned()
                || (physical["measured_algorithm_ns"].is_number_integer()
                    && physical["measured_algorithm_ns"].get<std::int64_t>()
                        >= 0))) {
            return false;
        }
        measured_sum += physical["measured_algorithm_ns"]
            .get<std::uint64_t>();
        if (postgres_sample) {
            if (!physical.value("statement_timeout_restored", false)
                || physical.value("statement_timeout_ms", std::uint64_t{})
                    != in.statement_timeout_ms
                || physical.value("postmaster_start_time", std::string{})
                    != out.timed_postmaster_start_time
                || physical.value("postgres_backend_pid", std::int64_t{-1})
                    != out.alignment_backend_pid
                || physical.value("database_oid", std::int64_t{-1})
                    != out.alignment_database_oid
                || physical.value("persisted_generation_id", std::int64_t{-1})
                    != out.persisted_generation_id
                || physical.value("tuple_accepted", 0) != 1
                || !physical.value(
                    "pipeline_validation_accepted", false)
                || !physical.value("stage_engine_matrix_valid", false)
                || !physical.contains("backend")
                || !physical["backend"].is_object()
                || physical["backend"].value("declines", -1) != 0
                || physical["backend"].value("fallbacks", -1) != 0
                || !physical.contains("alignment_snapshot")
                || !physical["alignment_snapshot"].is_object()
                || physical["alignment_snapshot"].value(
                    "actual_row_count", std::size_t{})
                    != e2e_output::k_alignment_snapshot_row_count
                || !physical.contains("persisted_gpu_operation_stats")
                || !physical["persisted_gpu_operation_stats"].is_object()
                || !physical.contains("interval_receipt")
                || !physical["interval_receipt"].is_object()
                || physical["interval_receipt"].value(
                    "available", std::int64_t{}) != 1
                || physical["interval_receipt"].value(
                    "failure_code", std::string{}) != "none") {
                return false;
            }
            const auto physical_union = physical["interval_receipt"].value(
                "union_ns", std::int64_t{-1});
            if (physical_union < 0) return false;
            union_sum += physical_union;
        } else {
            if (physical.value("child_pid", std::uint64_t{}) == 0
                || !fixed_lower_hex(
                    physical.value("receipt_checksum", std::string{}), 16)
                || !physical.contains("receipt_artifact")
                || !physical["receipt_artifact"].is_object()
                || physical["receipt_artifact"].value(
                    "path", std::string{}).empty()
                || !fixed_lower_hex(
                    physical["receipt_artifact"].value(
                        "digest", std::string{}), 16)
                || !physical.contains("validated_summary")
                || !physical["validated_summary"].is_object()) {
                return false;
            }
            const auto physical_union = physical.value(
                "combined_union_ns", std::uint64_t{});
            if (physical_union
                > physical["measured_algorithm_ns"].get<std::uint64_t>()) {
                return false;
            }
            union_sum += physical_union;
        }
    }
    const __int128 expected_union = postgres_sample
        ? static_cast<__int128>(out.benchmark_e2e_interval_union_ns)
        : static_cast<__int128>(out.minimap2_cigar_or_mapq_union_ns);
    return measured_sum == static_cast<__int128>(out.measured_region_ns)
        && union_sum == expected_union && union_sum <= measured_sum;
}

[[nodiscard]] std::string serialize_e2e_raw_sample(
        std::string_view algorithm,
        const parameter_map& parameters,
        const e2e_input& in,
        const e2e_output& out) {
    const bool postgres_sample = algorithm.starts_with("bioseq_")
        && (algorithm.ends_with("_no_index")
            || algorithm.ends_with("_just_index"));
    const bool matrix_sample =
        algorithm == bioseq::bench::e2e::detail::algorithm_matrix_just_index ||
        algorithm == bioseq::bench::e2e::detail::algorithm_matrix_no_index;
    const bool minimap2_sample = algorithm == "minimap2_just_index" ||
        algorithm == "minimap2_no_index";
    const bool attested_backend_sample =
        matrix_sample
        || algorithm.find("_scalar_") != std::string_view::npos
        || algorithm.find("_simd_") != std::string_view::npos
        || algorithm.find("_gpu_") != std::string_view::npos
        || algorithm.find("_vulkan_") != std::string_view::npos;
    const bool legacy_persisted_gpu_sample = postgres_sample
        && algorithm.ends_with("_just_index")
        && !matrix_sample
        && (algorithm.find("_gpu_") != std::string_view::npos
            || algorithm.find("_vulkan_") != std::string_view::npos);
    const bool hybrid_persisted_gpu_sample = postgres_sample
        && algorithm.ends_with("_just_index")
        && matrix_sample
        && (out.requested_minimizer_algorithm == "vulkan_minimap_integer"
            || out.requested_chaining_algorithm
                == "vulkan_tiled_dynamic_programming");
    const bool accepted_sample = out.tuple_accepted == 1;
    const bool skipped_after_prior_attestation_rejection =
        out.cell_disposition ==
            "skipped_after_prior_attestation_rejection";
    const bool has_prior_attestation_rejection_provenance =
        out.prior_attestation_rejection_source_repeat >= 0
        && !out.prior_attestation_rejection_reason.empty();
    const bool accepted_timing_available =
        accepted_sample && out.measured_region_available;
    const bool logical_chunked_sample = in.paired_minimap2_oracle
        && out.logical_chunking_enabled == 1;
    const bool exact_logical_chunk_receipt = logical_chunked_sample
        && exact_logical_chunk_receipt_valid(
            in, out, postgres_sample, minimap2_sample);
    const bool has_alignment_snapshot =
        out.alignment_snapshot_rows.size() == e2e_output::k_alignment_snapshot_row_count;
    const bool measured_query_connection_failure_invalid =
        out.tuple_accepted == 0
        && out.cell_disposition == "invalid"
        && out.phase == "measured_statement";
    const bool has_measured_query_connection_failure_proof =
        !out.measured_query_connection_failure_proof.is_null();
    if (skipped_after_prior_attestation_rejection
            != has_prior_attestation_rejection_provenance
        || (skipped_after_prior_attestation_rejection
            && (!matrix_sample || out.tuple_accepted != 0
                || out.phase != "not_executed"
                || out.correctness_evaluated != 0
                || out.tuple_rejection_reason
                    != out.prior_attestation_rejection_reason
                || out.measured_region_available
                || !out.alignment_snapshot_rows.empty()
                || out.ordered_query_oracle_available != 0
                || out.pipeline_stats_available != 0
                || out.alignment_execution_id != -1
                || out.target_rows != -1
                || out.attempted_alignment_jobs != -1
                || out.benchmark_e2e_interval_available != -1
                || !out.benchmark_e2e_interval_origins.is_null()
                || out.minimap2_receipt_available != -1
                || !out.persisted_gpu_operation_stats.is_null()
                || !out.gate8_sql_proof.is_null()
                || !out.sql_result_status.empty()
                || !out.sqlstate.empty()
                || !out.sql_primary.empty()
                || !out.sql_detail.empty()
                || !out.sql_hint.empty()
                || !out.sql_connection_diagnostic.empty()
                || has_measured_query_connection_failure_proof))) {
        abort_raw_stats_serialization(
            "prior-attestation skip retained execution evidence or lacks provenance");
    }
    if (measured_query_connection_failure_invalid
            != has_measured_query_connection_failure_proof
        || (has_measured_query_connection_failure_proof
            && (!postgres_sample || has_alignment_snapshot))) {
        abort_raw_stats_serialization(
            "measured-query connection failure lacks its exclusive PostgreSQL proof");
    }
    if (postgres_sample && accepted_sample && !exact_logical_chunk_receipt
        && out.alignment_snapshot_rows.size()
            != e2e_output::k_alignment_snapshot_row_count) {
        abort_raw_stats_serialization(
            "bioseq sample has no complete same-invocation SRF stats");
    }
    if (attested_backend_sample && accepted_sample
        && out.pipeline_stats_consistent != 1) {
        abort_raw_stats_serialization(
            "selected backend sample was not accepted before serialization");
    }
    if (postgres_sample && accepted_sample && !exact_logical_chunk_receipt
        && (out.persisted_gpu_operation_stats_schema_valid != 1
            || out.persisted_gpu_operation_stats.is_null())) {
        abort_raw_stats_serialization(
            "PostgreSQL sample is missing current GPU operation stats");
    }
    const bool minimizer_fragment_reader_stats_valid = json_has_exact_keys(
            out.minimizer_fragment_reader_stats,
            k_minimizer_fragment_reader_stats_keys)
        && out.minimizer_fragment_reader_stats["schema"].is_string()
        && out.minimizer_fragment_reader_stats["schema"]
            == "bioseq.grouped_soa_reader_memory_stats"
        && out.minimizer_fragment_reader_stats["revision"].is_number_integer()
        && out.minimizer_fragment_reader_stats["revision"] == 6;
    if (postgres_sample && accepted_sample && !exact_logical_chunk_receipt
        && !minimizer_fragment_reader_stats_valid) {
        abort_raw_stats_serialization(
            "PostgreSQL sample is missing exact minimizer-fragment reader stats schema");
    }
    const bool hash_major_index_reader_stats_valid = json_has_exact_keys(
            out.hash_major_index_reader_stats,
            k_hash_major_index_reader_stats_keys)
        && out.hash_major_index_reader_stats["schema"].is_string()
        && out.hash_major_index_reader_stats["schema"]
            == "bioseq.hash_major_reader_stats"
        && out.hash_major_index_reader_stats["revision"].is_number_integer()
        && out.hash_major_index_reader_stats["revision"] == 1;
    if (postgres_sample && accepted_sample && !exact_logical_chunk_receipt
        && !hash_major_index_reader_stats_valid) {
        abort_raw_stats_serialization(
            "PostgreSQL sample is missing exact hash-major index reader stats schema");
    }
    if (legacy_persisted_gpu_sample && accepted_sample
        && !out.has_accepted_legacy_persisted_gpu_operation_ledger(
            out.attempted_alignment_jobs)) {
        abort_raw_stats_serialization(
            "persisted all-GPU sample lacks accepted operation stats");
    }
    if (hybrid_persisted_gpu_sample && accepted_sample
        && !out.has_exact_empty_persisted_gpu_operation_ledger()) {
        abort_raw_stats_serialization(
            "persisted hybrid Vulkan sample retained legacy all-GPU operation stats");
    }
    if (postgres_sample && accepted_sample && algorithm.ends_with("_no_index")
        && out.cold_no_index_lifecycle_accepted != 1) {
        abort_raw_stats_serialization(
            "no_index sample lacks accepted cold lifecycle stats");
    }
    if (postgres_sample && accepted_sample && algorithm.ends_with("_just_index")
        && out.persisted_policy_audit_accepted != 1) {
        abort_raw_stats_serialization(
            "just_index sample lacks accepted post-measure persisted-policy stats");
    }
    if (matrix_sample && accepted_sample &&
        (out.benchmark_e2e_interval_schema_revision != 6 ||
         out.benchmark_e2e_interval_available != 1 ||
         out.benchmark_e2e_interval_finalization_count
            != (logical_chunked_sample ? out.logical_chunk_count : 1) ||
         out.benchmark_e2e_interval_failure_code != "none" ||
         out.benchmark_e2e_interval_union_ns < 0 ||
         out.benchmark_e2e_interval_origins.empty())) {
        abort_raw_stats_serialization(
            "algorithm-matrix row lacks an accepted revision-6 interval union");
    }
    // The coverage campaign deliberately does not execute the extra Gate-8
    // mapping controls. Measured-statement, correctness, stage, cold-lifecycle
    // and interval checks above still apply; absent control proof is null.
    if (!out.gate8_sql_proof.is_null()) {
        abort_raw_stats_serialization(
            "control-free coverage sample unexpectedly contains Gate-8 SQL proof");
    }
    if (minimap2_sample && accepted_sample &&
        (out.minimap2_receipt_available != 1 ||
         out.minimap2_receipt_pid <= 0 ||
         out.minimap2_cigar_interval_count <= 0 ||
         out.minimap2_mapq_interval_count <= 0 ||
         out.minimap2_cigar_or_mapq_union_ns < 0 ||
         !fixed_lower_hex(out.minimap2_receipt_checksum, 16) ||
         !fixed_lower_hex(out.minimap2_receipt_artifact_digest, 16) ||
         out.minimap2_receipt_artifact_path.empty() ||
         out.minimap2_receipt_summary.is_null())) {
        abort_raw_stats_serialization(
            "minimap2 row lacks an exact validated phase receipt");
    }
    if (logical_chunked_sample && accepted_sample
        && !exact_logical_chunk_receipt) {
        abort_raw_stats_serialization(
            "logical paired row lacks an exact complete bounded-chunk receipt");
    }
    if (in.paired_minimap2_oracle && accepted_sample
        && (out.ordered_query_oracle_available != 1
            || out.ordered_query_oracle.size() != in.queries.size()
            || !fixed_lower_hex(out.ordered_query_oracle_sha256, 64)
            || !fixed_lower_hex(out.ordered_query_mapq_sha256, 64)
            || out.paired_oracle_repeat_ordinal < 0
            || out.paired_oracle_comparison_available != 1
            || out.paired_oracle_exact != 1
            || out.paired_oracle_verdict_sealed != 1
            || out.paired_mapq_vector_equal < 0
            || !fixed_lower_hex(
                out.paired_generated_target_content_crc64, 16)
            || !fixed_lower_hex(
                out.paired_persisted_target_content_crc64, 16)
            || out.paired_persisted_target_content_crc64
                != out.paired_generated_target_content_crc64)) {
        abort_raw_stats_serialization(
            "paired row lacks exact target identity, ordered primary-count "
            "oracle verdict, or diagnostic MAPQ evidence");
    }
    if (!in.paired_minimap2_oracle && accepted_sample
        && (matrix_sample || minimap2_sample)
        && (out.ordered_query_oracle_available != 1
            || out.ordered_query_oracle.size() != in.queries.size()
            || !fixed_lower_hex(out.ordered_query_oracle_sha256, 64)
            || !fixed_lower_hex(out.ordered_query_mapq_sha256, 64)
            || out.paired_oracle_repeat_ordinal != -1
            || out.paired_oracle_comparison_available != 0
            || out.paired_oracle_exact != -1
            || out.paired_oracle_verdict_sealed != 0
            || out.paired_mapq_vector_equal != -1)) {
        abort_raw_stats_serialization(
            "ordinary matrix row lacks its unsealed authenticated ordered "
            "primary-count vector or falsely claims a paired verdict");
    }
    if (in.require_nice_attestation && accepted_sample
        && (out.priority_target_nice != k_normal_user_best_nice
            || out.runner_pid <= 0
            || out.runner_nice != k_normal_user_best_nice
            || out.priority_attestation_accepted != 1
            || (postgres_sample
                && out.postgres_backend_nice != k_normal_user_best_nice))) {
        abort_raw_stats_serialization(
            "accepted row lacks exact runner/backend nice attestation");
    }
    // A correctness-invalid row may have completed interval collection before
    // the post-measurement parity verdict.  Do not serialize that receipt (or
    // any duration derived from it) as admissible evidence once the tuple is
    // rejected.
    const bool bioseq_interval_source = accepted_sample &&
        out.benchmark_e2e_interval_available == 1;
    const bool minimap2_interval_source = accepted_sample &&
        out.minimap2_receipt_available == 1;
    const std::int64_t cigar_or_mapq_interval_union_ns =
        bioseq_interval_source ? out.benchmark_e2e_interval_union_ns
        : minimap2_interval_source ? out.minimap2_cigar_or_mapq_union_ns : -1;
    if ((matrix_sample || minimap2_sample) && accepted_sample &&
        (bioseq_interval_source == minimap2_interval_source ||
         !accepted_timing_available || cigar_or_mapq_interval_union_ns < 0 ||
         static_cast<std::uint64_t>(cigar_or_mapq_interval_union_ns) >
             out.measured_region_ns)) {
        abort_raw_stats_serialization(
            "campaign row has no unique same-run CIGAR-or-MAPQ interval union");
    }

    nlohmann::json payload = nlohmann::json::object();
    payload["payload_schema"] = "bioseq.e2e.raw-sample";
    payload["payload_schema_version"] =
        bioseq::bench::framework::e2e_raw_payload_schema_version;
    payload["gate8_sql"] = out.gate8_sql_proof;
    if (skipped_after_prior_attestation_rejection) {
        payload["prior_attestation_rejection"] = nlohmann::json{
            {"schema", "bioseq.e2e.prior-attestation-rejection"},
            {"version", 1},
            {"source_repeat",
                out.prior_attestation_rejection_source_repeat},
            {"source_disposition", "rejected_attestation"},
            {"source_rejection_reason",
                out.prior_attestation_rejection_reason},
        };
    }
    const std::string pairing_mode = algorithm.ends_with("_just_index")
        ? std::string{"just_index"}
        : algorithm.ends_with("_no_index")
            ? std::string{"no_index"}
            : std::string{"other"};
    const std::string pairing_index = pairing_mode == "no_index"
        ? "none"
        : algorithm.starts_with("minimap2_")
            ? "minimap2_mmi"
            : param_string(parameters, "minimizer_index", "not_applicable");
    // The framework envelope adds the exact logical `repeat`, test-case, and
    // resolved parameter map around this payload. These stable payload keys
    // make the post-run minimap2 parity join explicit without replaying work.
    payload["pairing"] = nlohmann::json{
        {"dataset_key", in.size_label},
        {"alphabet", in.alphabet},
        {"target_size_bp", in.target_bp},
        {"kmer_size", in.kmer_size},
        {"window_size", in.window_size},
        {"mode", pairing_mode},
        {"index_key", pairing_index},
        {"repeat_field", "framework_envelope.repeat"},
        {"generated_target_content_crc64",
            in.paired_minimap2_oracle
                ? nlohmann::json(
                    out.paired_generated_target_content_crc64)
                : nlohmann::json(nullptr)},
        {"persisted_source_content_crc64",
            !out.paired_persisted_target_content_crc64.empty()
                ? nlohmann::json(
                    out.paired_persisted_target_content_crc64)
                : nlohmann::json(nullptr)},
        {"generated_target_matches_persisted_source",
            !out.paired_persisted_target_content_crc64.empty()
                ? nlohmann::json(
                    out.paired_persisted_target_content_crc64
                        == out.paired_generated_target_content_crc64)
                : nlohmann::json(nullptr)},
        {"minimap2_setup_receipt",
            in.paired_minimap2_oracle
                ? nlohmann::json{
                    {"checksum", in.minimap2_setup_receipt_checksum},
                    {"artifact_path",
                        in.minimap2_setup_receipt_artifact_path},
                    {"artifact_digest",
                        in.minimap2_setup_receipt_artifact_digest},
                    {"validated_summary",
                        in.minimap2_setup_receipt_summary},
                    {"preserved_external_evidence", true},
                }
                : nlohmann::json(nullptr)},
    };
    payload["ordered_query_oracle"] =
        out.ordered_query_oracle_available == 1
            ? nlohmann::json{
                {"schema", "bioseq.e2e.ordered-query-primary-count-oracle"},
                {"version", 2},
                {"ordered_query_count", out.ordered_query_oracle.size()},
                {"fields", nlohmann::json::array({
                    "query_id", "primary_match_count"})},
                {"digest_domain", std::string{
                    k_ordered_query_oracle_digest_domain}},
                {"digest_algorithm", "sha256"},
                {"digest", out.ordered_query_oracle_sha256},
                {"digest_words", nlohmann::json{
                    {"word_0", out.ordered_query_oracle_sha256_words[0]},
                    {"word_1", out.ordered_query_oracle_sha256_words[1]},
                    {"word_2", out.ordered_query_oracle_sha256_words[2]},
                    {"word_3", out.ordered_query_oracle_sha256_words[3]},
                    {"word_4", out.ordered_query_oracle_sha256_words[4]},
                    {"word_5", out.ordered_query_oracle_sha256_words[5]},
                    {"word_6", out.ordered_query_oracle_sha256_words[6]},
                    {"word_7", out.ordered_query_oracle_sha256_words[7]},
                }},
                {"ordered_observations", [&out] {
                    nlohmann::json observations = nlohmann::json::array();
                    for (const auto& observation :
                         out.ordered_query_oracle) {
                        observations.push_back(nlohmann::json{
                            {"query_id", observation.query_id},
                            {"primary_match_count",
                                observation.primary_match_count},
                        });
                    }
                    return observations;
                }()},
                {"raw_observations_embedded", true},
                {"repeat_ordinal", out.paired_oracle_repeat_ordinal >= 0
                    ? nlohmann::json(out.paired_oracle_repeat_ordinal)
                    : nlohmann::json(nullptr)},
                {"direct_comparison_available",
                    out.paired_oracle_comparison_available == 1},
                {"direct_comparison_exact",
                    out.paired_oracle_comparison_available == 1
                        ? nlohmann::json(out.paired_oracle_exact == 1)
                        : nlohmann::json(nullptr)},
                {"validity_scope",
                    "exact ordered query-id and primary-match-count equality "
                    "plus exact total primary-match-count equality"},
                {"mapq_participates_in_validity", false},
                {"pair_verdict_sealed",
                    out.paired_oracle_verdict_sealed == 1},
                {"diagnostic", out.paired_oracle_diagnostic.empty()
                    ? nlohmann::json(nullptr)
                    : nlohmann::json(out.paired_oracle_diagnostic)},
            }
            : nlohmann::json(nullptr);
    payload["mapq_diagnostic"] =
        out.ordered_query_oracle_available == 1
            ? nlohmann::json{
                {"schema", "bioseq.e2e.ordered-query-mapq-diagnostic"},
                {"version", 1},
                {"ordered_query_count", out.ordered_query_oracle.size()},
                {"fields", nlohmann::json::array({"query_id", "mapq"})},
                {"digest_domain", std::string{
                    k_ordered_query_mapq_digest_domain}},
                {"digest_algorithm", "sha256"},
                {"digest", out.ordered_query_mapq_sha256},
                {"digest_words", nlohmann::json{
                    {"word_0", out.ordered_query_mapq_sha256_words[0]},
                    {"word_1", out.ordered_query_mapq_sha256_words[1]},
                    {"word_2", out.ordered_query_mapq_sha256_words[2]},
                    {"word_3", out.ordered_query_mapq_sha256_words[3]},
                    {"word_4", out.ordered_query_mapq_sha256_words[4]},
                    {"word_5", out.ordered_query_mapq_sha256_words[5]},
                    {"word_6", out.ordered_query_mapq_sha256_words[6]},
                    {"word_7", out.ordered_query_mapq_sha256_words[7]},
                }},
                {"ordered_vector", [&out] {
                    nlohmann::json vector = nlohmann::json::array();
                    for (const auto& observation :
                         out.ordered_query_oracle) {
                        vector.push_back(nlohmann::json{
                            {"query_id", observation.query_id},
                            {"mapq", observation.mapq},
                        });
                    }
                    return vector;
                }()},
                {"raw_vector_embedded", true},
                {"numeric_vector_equal",
                    out.paired_mapq_vector_equal >= 0
                        ? nlohmann::json(out.paired_mapq_vector_equal == 1)
                        : nlohmann::json(nullptr)},
                {"numeric_equality_is_validity_gate", false},
                {"minimap2_mapq_semantics_claimed", false},
                {"diagnostic", out.paired_mapq_diagnostic.empty()
                    ? nlohmann::json(nullptr)
                    : nlohmann::json(out.paired_mapq_diagnostic)},
            }
            : nlohmann::json(nullptr);
    payload["logical_execution"] = logical_chunked_sample
        ? nlohmann::json{
            {"schema", "bioseq.e2e.logical-query-chunks"},
            {"version", 1},
            {"query_count", in.queries.size()},
            {"chunk_size", out.logical_chunk_size},
            {"chunk_count", out.logical_chunk_count},
            {"statement_timeout_ms",
                out.logical_statement_timeout_ms},
            {"statement_timeout_restored",
                out.logical_statement_timeout_restored == 1},
            {"deterministic_order",
                "contiguous_global_query_id_ascending"},
            {"one_logical_sample", true},
            {"pair_atomic_verdict",
                out.paired_oracle_verdict_sealed == 1},
            {"published_duration_source",
                "sum_of_physical_algorithm_envelopes_only"},
            {"validation_excluded_from_published_duration", true},
            {"validation_excluded_ns", out.validation_excluded_ns},
            {"logical_receipt", out.logical_chunk_receipts},
        }
        : nlohmann::json(nullptr);
    payload["process_priority_attestation"] =
        !skipped_after_prior_attestation_rejection
            && in.require_nice_attestation
            ? nlohmann::json{
                {"schema", "bioseq.e2e.process-priority"},
                {"version", 1},
                {"policy", "highest_prompt_free_normal_user_priority"},
                {"periodic_sampling", false},
                {"target_nice", out.priority_target_nice},
                {"runner_pid", out.runner_pid},
                {"runner_nice", out.runner_nice},
                {"postgres_backend_pid", out.post_restart_backend_pid > 0
                    ? nlohmann::json(out.post_restart_backend_pid)
                    : out.cold_no_index_post_restart_backend_pid > 0
                        ? nlohmann::json(
                            out.cold_no_index_post_restart_backend_pid)
                        : nlohmann::json(nullptr)},
                {"postgres_backend_nice", out.postgres_backend_nice >= 0
                    ? nlohmann::json(out.postgres_backend_nice)
                    : nlohmann::json(nullptr)},
                {"accepted", out.priority_attestation_accepted == 1},
                {"capture_position", "outside_measured_region"},
            }
            : nlohmann::json(nullptr);
    // Kept for established benchmark-e2e report compatibility. Campaign
    // Metric A is exclusively the framework envelope's outer_e2e_ns below.
    payload["index_and_map_ns"] = accepted_timing_available
        ? nlohmann::json(out.measured_region_ns) : nlohmann::json(nullptr);
    payload["cigar_or_mapq_interval_union_ns"] =
        accepted_timing_available && cigar_or_mapq_interval_union_ns >= 0
            ? nlohmann::json(cigar_or_mapq_interval_union_ns)
            : nlohmann::json(nullptr);
    payload["e2e_interval_metric_contract"] = nlohmann::json{
        {"schema", "bioseq.e2e.interval-metrics"},
        {"version", 2},
        {"framework_finalize_adjusted_metric",
            matrix_sample || minimap2_sample},
        {"metric_a", nlohmann::json{
            {"name", "outer_e2e_ns"},
            {"source", "framework_envelope.outer_e2e_ns"},
            {"aggregation", "p50_after_repeat_collection"},
        }},
        {"metric_b", nlohmann::json{
            {"name", "outer_e2e_minus_cigar_or_mapq_union_ns"},
            {"per_repeat_formula",
                "framework_envelope.outer_e2e_ns-cigar_or_mapq_interval_union_ns"},
            {"aggregation", "p50_after_per_repeat_subtraction"},
            {"forbidden_derivations", nlohmann::json::array({
                "subtract_medians", "sum_per_kind_intervals",
                "subtract_downstream_active_wall"
            })},
        }},
        {"validated_inner_envelope", nlohmann::json{
            {"name", "measured_region_ns_manual"},
            {"value_ns", accepted_timing_available
                ? nlohmann::json(out.measured_region_ns)
                : nlohmann::json(nullptr)},
            {"union_not_greater", accepted_timing_available &&
                cigar_or_mapq_interval_union_ns >= 0 &&
                static_cast<std::uint64_t>(cigar_or_mapq_interval_union_ns) <=
                    out.measured_region_ns},
        }},
        {"duration_clock_domains", nlohmann::json{
            {"framework_outer", nlohmann::json{
                {"clock", "bioseq.instrumentation.clock::now_ns"},
                {"unit", "ns"},
                {"value_kind", "duration"},
            }},
            {"interval_union", nlohmann::json{
                {"clock", bioseq_interval_source
                    ? "bioseq.instrumentation.clock::now_ns"
                    : minimap2_interval_source
                        ? "helper-declared CLOCK_MONOTONIC"
                        : "not_available"},
                {"unit", "ns"},
                {"value_kind", "duration"},
                {"rate_compatibility", bioseq_interval_source
                    ? "same_clock_implementation"
                    : minimap2_interval_source
                        ? std::string{
                            k_minimap2_framework_duration_rate_contract}
                        : "not_available"},
            }},
            {"subtraction_basis",
                "nonnegative monotonic nanosecond durations with "
                "union_ns<=same_repeat_outer_e2e_ns"},
            {"absolute_timestamp_domain_equality_claimed", false},
            {"same_repeat_proof", "framework_program_order_and_raw_envelope"},
        }},
        {"framework_acceptance_gate",
            "framework_finalizer_requires_accepted_tuple,proven_duration_rate,"
            "union_ns<=same_repeat_outer_e2e_ns"},
    };
    payload["downstream_active_wall_available"] =
        out.downstream_active_wall_available >= 0
            ? nlohmann::json(out.downstream_active_wall_available == 1)
            : nlohmann::json(nullptr);
    payload["downstream_active_wall_ns"] =
        accepted_timing_available
            && out.downstream_active_wall_available == 1
            ? nlohmann::json(out.downstream_active_wall_ns)
            : nlohmann::json(nullptr);
    payload["downstream_active_wall_invocation_count"] =
        out.downstream_active_wall_invocation_count >= 0
            ? nlohmann::json(out.downstream_active_wall_invocation_count)
            : nlohmann::json(nullptr);
    payload["downstream_active_wall_unavailable_invocation_count"] =
        out.downstream_active_wall_unavailable_invocation_count >= 0
            ? nlohmann::json(
                out.downstream_active_wall_unavailable_invocation_count)
            : nlohmann::json(nullptr);
    payload["downstream_work_ns"] = accepted_timing_available
        && out.downstream_work_ns >= 0
        ? nlohmann::json(out.downstream_work_ns) : nlohmann::json(nullptr);
    payload["downstream_work_saturated"] = out.downstream_work_saturated >= 0
        ? nlohmann::json(out.downstream_work_saturated == 1)
        : nlohmann::json(nullptr);
    payload["interval_receipt"] = bioseq_interval_source
        ? nlohmann::json{
            {"source", "bioseq_postgresql_revision6"},
            {"stats_revision", out.benchmark_e2e_interval_schema_revision},
            {"available", true},
            {"union_ns", out.benchmark_e2e_interval_union_ns},
            {"interval_count", out.benchmark_e2e_interval_count},
            {"cigar_interval_count",
                out.benchmark_e2e_cigar_interval_count},
            {"mapq_interval_count", out.benchmark_e2e_mapq_interval_count},
            {"finalization_count",
                out.benchmark_e2e_interval_finalization_count},
            {"failure_code", out.benchmark_e2e_interval_failure_code},
            {"physical_interval_semantics",
                "half_open_monotonic_same_statement_union"},
            {"origins", out.benchmark_e2e_interval_origins},
        }
        : minimap2_interval_source
            ? nlohmann::json{
                {"source", "minimap2_phase_receipt_v2"},
                {"available", true},
                {"union_ns", out.minimap2_cigar_or_mapq_union_ns},
                {"cigar_union_ns", out.minimap2_cigar_union_ns},
                {"mapq_union_ns", out.minimap2_mapq_union_ns},
                {"cigar_interval_count", out.minimap2_cigar_interval_count},
                {"mapq_interval_count", out.minimap2_mapq_interval_count},
                {"child_pid", out.minimap2_receipt_pid},
                {"checksum", out.minimap2_receipt_checksum},
                {"artifact", nlohmann::json{
                    {"path", out.minimap2_receipt_artifact_path},
                    {"digest_algorithm", "fnv-1a-64_raw_receipt_bytes"},
                    {"digest", out.minimap2_receipt_artifact_digest},
                }},
                {"validated_summary", out.minimap2_receipt_summary},
                {"raw_intervals_embedded", false},
            }
            : nlohmann::json(nullptr);
    payload["tuple_status"] = nlohmann::json{
        {"accepted", out.tuple_accepted},
        {"disposition", out.cell_disposition},
        {"phase", out.phase},
        {"correctness_evaluated", out.correctness_evaluated},
        {"rejection_reason", out.tuple_rejection_reason},
        {"behavior_difference_comment", out.behavior_difference_comment},
        {"requested_minimizer_algorithm", out.requested_minimizer_algorithm},
        {"requested_chaining_algorithm", out.requested_chaining_algorithm},
        {"requested_base_alignment_algorithm", out.requested_base_alignment_algorithm},
        // Only fields that participate in the exact rejection allowlist enter
        // campaign evidence. Hint and connection text remain local diagnostics;
        // serializing them without exact validation would widen identity.
        {"sql_diagnostics", nlohmann::json{
            {"result_status", out.sql_result_status},
            {"sqlstate", out.sqlstate},
            {"primary", out.sql_primary},
            {"detail", out.sql_detail},
        }},
    };
    payload["cache_policy"] = "process-cold-os-cache-unspecified";
    payload["benchmark_shape"] = nlohmann::json{
        {"expected_target_rows", postgres_sample
            ? nlohmann::json(1) : nlohmann::json(nullptr)},
        {"expected_query_rows", in.queries.size()},
        {"expected_alignment_jobs", in.queries.size()},
        {"actual_target_rows",
            has_alignment_snapshot || exact_logical_chunk_receipt
            ? nlohmann::json(out.target_rows) : nlohmann::json(nullptr)},
        {"actual_alignment_jobs",
            has_alignment_snapshot || exact_logical_chunk_receipt
            ? nlohmann::json(out.attempted_alignment_jobs)
            : nlohmann::json(nullptr)},
    };
    payload["correctness"] = skipped_after_prior_attestation_rejection
        ? nlohmann::json(nullptr)
        : nlohmann::json{
        {"evaluated", out.correctness_evaluated},
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
        {"mapq_result_count", out.mapq_result_count},
        {"max_abs_offset_bp", out.position_offset_available
                && std::isfinite(out.max_abs_offset_bp)
            ? nlohmann::json(out.max_abs_offset_bp)
            : nlohmann::json(nullptr)},
    };
    const auto absence_stats_json = [](
            const cold_no_index_policy_absence_stats& stats) {
        return nlohmann::json{
            {"target_rows", stats.target_rows},
            {"target_cardinality_metadata_only",
                stats.target_cardinality_metadata_only},
            {"source_parent_rows_read", stats.source_parent_rows_read},
            {"source_fragment_rows_read", stats.source_fragment_rows_read},
            {"source_decoded_bp", stats.source_decoded_bp},
            {"physical_index_relations", stats.physical_index_relations},
            {"persisted_index_rows", stats.persisted_index_rows},
            {"generation_objects", stats.generation_objects},
        };
    };

    if (has_measured_query_connection_failure_proof) {
        payload["postgres"] = nlohmann::json{
            {"measured_query_connection_failure",
                out.measured_query_connection_failure_proof},
        };
        payload["alignment_snapshot"] = nullptr;
        payload["persisted_gpu_operation_stats"] = nullptr;
    } else if (exact_logical_chunk_receipt && postgres_sample) {
        payload["postgres"] = nlohmann::json{
            {"logical_chunked_execution", true},
            {"physical_chunk_count", out.logical_chunk_count},
            {"generation_id", out.persisted_generation_id},
            {"source_identity", out.persisted_source_identity},
            {"device_uuid", out.gpu_device_uuid},
            {"driver", out.gpu_driver},
            {"pipeline_cache_manifest_checksum",
                out.gpu_pipeline_cache_manifest_checksum},
            {"same_ready_fixture_reused", true},
            {"physical_alignment_snapshots_embedded_in",
                "logical_execution.logical_receipt.physical_chunks"},
        };
        payload["alignment_snapshot"] = nullptr;
        payload["persisted_gpu_operation_stats"] =
            out.persisted_gpu_operation_stats;
    } else if (has_alignment_snapshot) {
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
                    {"rows", out.audit_minimizer_fragment_rows},
                    {"update_on_insert", out.audit_minimizer_fragment_update_on_insert},
                    {"active_read", out.audit_minimizer_fragment_active_read},
                }},
                {"minimizers_per_sequence", nlohmann::json{
                    {"rows", out.audit_minimizer_sequence_rows},
                    {"update_on_insert", out.audit_minimizer_sequence_update_on_insert},
                    {"active_read", out.audit_minimizer_sequence_active_read},
                }},
                {"async_persist_jobs_waiting",
                    out.audit_async_persist_jobs_waiting},
                {"index_rows_wait_attempts", out.audit_index_rows_wait_attempts},
            }
            : nlohmann::json(nullptr);
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
            {"persisted_layout", just_index
                ? nlohmann::json{
                    {"layout", out.persisted_layout},
                    {"pre_restart_layout",
                        out.pre_restart_persisted_layout},
                    {"post_restart_layout",
                        out.post_restart_persisted_layout},
                    {"ready_generation_gpu_abi_version",
                        out.pre_restart_persisted_gpu_abi_version},
                    {"manifest_rows_read",
                        out.persisted_manifest_rows_read},
                    {"manifest_bytes_read",
                        out.persisted_manifest_bytes_read},
                    {"read_sessions",
                        out.persisted_generation_read_sessions},
                    {"grouped_soa_manifest_contract_valid",
                        out.persisted_format5_manifest_contract_valid},
                    {"hash_major_contract_valid",
                        out.persisted_format6_contract_valid},
                    {"format6_restart_audit_accepted",
                        out.format6_restart_audit_accepted},
                    {"hash_major_pre_restart_ready_stats",
                        out.pre_restart_hash_major_ready_stats},
                    {"hash_major_post_restart_ready_stats",
                        out.post_restart_hash_major_ready_stats},
                }
                : nlohmann::json(nullptr)},
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
                    {"pre_restart_minimizer_fragment_rows",
                        out.cold_no_index_pre_restart_minimizer_fragment_rows},
                    {"pre_restart_minimizer_sequence_rows",
                        out.cold_no_index_pre_restart_minimizer_sequence_rows},
                    {"post_restart_minimizer_fragment_rows",
                        out.cold_no_index_post_restart_minimizer_fragment_rows},
                    {"post_restart_minimizer_sequence_rows",
                        out.cold_no_index_post_restart_minimizer_sequence_rows},
                    {"post_measure_minimizer_fragment_rows",
                        out.cold_no_index_measured_minimizer_fragment.persisted_index_rows},
                    {"post_measure_minimizer_sequence_rows",
                        out.cold_no_index_measured_minimizer_sequence.persisted_index_rows},
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
                    {"pre_restart_minimizer_fragment_absence", absence_stats_json(
                        out.cold_no_index_pre_restart_minimizer_fragment)},
                    {"pre_restart_minimizer_sequence_absence", absence_stats_json(
                        out.cold_no_index_pre_restart_minimizer_sequence)},
                    {"post_restart_minimizer_fragment_absence", absence_stats_json(
                        out.cold_no_index_post_restart_minimizer_fragment)},
                    {"post_restart_minimizer_sequence_absence", absence_stats_json(
                        out.cold_no_index_post_restart_minimizer_sequence)},
                    {"post_measure_minimizer_fragment_absence", absence_stats_json(
                        out.cold_no_index_measured_minimizer_fragment)},
                    {"post_measure_minimizer_sequence_absence", absence_stats_json(
                        out.cold_no_index_measured_minimizer_sequence)},
                }
                : nlohmann::json(nullptr)},
        };
        payload["alignment_snapshot"] =
            serialize_alignment_snapshot_rows(out, accepted_sample);
        payload["persisted_gpu_operation_stats"] =
            out.persisted_gpu_operation_stats;
    } else {
        payload["postgres"] = nullptr;
        payload["alignment_snapshot"] = nullptr;
        payload["persisted_gpu_operation_stats"] = nullptr;
    }
    payload["attestation"] = skipped_after_prior_attestation_rejection
        ? nlohmann::json(nullptr)
        : nlohmann::json{
        {"validation_present", out.pipeline_stats_available},
        {"validation_attached_to_measurement", out.pipeline_stats_attached_to_measurement},
        {"validation_accepted", out.pipeline_stats_consistent},
        {"stage_engine_matrix_valid", out.stage_engine_matrix_valid},
        {"requested_backend", out.requested_backend},
        {"actual_backend", out.actual_backend},
        {"requested_minimizer_algorithm", out.attested_requested_minimizer_algorithm},
        {"observed_minimizer_algorithm", out.observed_minimizer_algorithm},
        {"requested_chaining_algorithm", out.attested_requested_chaining_algorithm},
        {"observed_chaining_algorithm", out.observed_chaining_algorithm},
        {"requested_base_alignment_algorithm", out.attested_requested_base_alignment_algorithm},
        {"observed_base_alignment_algorithm", out.observed_base_alignment_algorithm},
        {"requested_target_index_source", out.requested_target_index_source},
        {"actual_target_index_source", out.actual_target_index_source},
        {"requested_simd_chaining_policy", out.requested_simd_chaining_policy},
        {"observed_simd_chaining_policy", out.observed_simd_chaining_policy},
        {"requested_simd_alignment_algorithm", out.requested_simd_alignment_algorithm},
        {"observed_simd_alignment_algorithm", out.observed_simd_alignment_algorithm},
        {"mapq_execution_available",
         out.mapq_execution_available >= 0 ? nlohmann::json(out.mapq_execution_available == 1)
                                           : nlohmann::json(nullptr)},
        {"mapq_executed",
         out.mapq_execution_available == 1 ? nlohmann::json(out.mapq_executed == 1)
                                           : nlohmann::json(nullptr)},
        {"mapq_execution_conflict",
         out.mapq_execution_conflict >= 0 ? nlohmann::json(out.mapq_execution_conflict == 1)
                                          : nlohmann::json(nullptr)},
        {"vulkan_physical_mapq_executed",
         out.observed_base_alignment_algorithm == "vulkan_simple_cigar"
             ? nlohmann::json(in.mapq_projection && out.mapq_execution_available == 1 &&
                              out.mapq_executed == 1 && out.mapq_execution_conflict == 0 &&
                              out.benchmark_e2e_mapq_interval_count > 0)
             : nlohmann::json(nullptr)},
        {"numeric_mapq_matches_minimap2_semantics_claimed", false},
        {"documented_minimizer_fallback_selected",
         out.attested_requested_minimizer_algorithm == "simd_minimap2_reference" &&
             out.observed_minimizer_algorithm == "scalar"},
        {"nthash_vector_activity",
         out.nthash_vector_batches > 0 || out.nthash_resident_vector_stream_kernel_calls > 0},
        {"block_aligner_execution_stats",
         nlohmann::json{
             {"available", out.block_aligner_execution_stats_available},
             {"consistent", out.block_aligner_execution_stats_consistent},
             {"executed_calls", out.block_aligner_executed_calls},
             {"nonempty_calls", out.block_aligner_nonempty_calls},
             {"nonempty_calls_with_simd", out.block_aligner_nonempty_calls_with_simd},
             {"simd_dp_vector_batches", out.block_aligner_simd_dp_vector_batches},
             {"simd_dp_cells", out.block_aligner_simd_dp_cells},
             {"forbidden_ksw2_calls", out.block_aligner_forbidden_ksw2_calls},
             {"forbidden_scalar_biological_fallbacks",
              out.block_aligner_forbidden_scalar_biological_fallbacks},
             {"saturated", out.block_aligner_execution_stats_saturated},
         }},
        {"prefilter_execution_stats", out.prefilter_execution_stats},
        {"backend_declines", out.backend_declines},
        {"backend_fallbacks", out.backend_fallbacks},
        {"stats_saturated", out.stats_saturated},
        {"persisted_gpu_operation_stats_accepted",
         postgres_sample ? nlohmann::json(out.persisted_gpu_operation_stats_accepted)
                         : nlohmann::json(nullptr)},
        {"minimizer_fragment_reader_stats",
         postgres_sample ? out.minimizer_fragment_reader_stats : nlohmann::json(nullptr)},
        {"hash_major_index_reader_stats",
         postgres_sample ? out.hash_major_index_reader_stats : nlohmann::json(nullptr)},
    };
    sanitize_e2e_raw_numbers(payload, !accepted_sample);
    return payload.dump();
}
/** @brief Table state and expected order for the SQL sort benchmark. */
struct sort_scan_input {
    std::shared_ptr<it::connection_test_context> conn;
    std::string table;
    std::string seq_type = "bioseq.seq('dna4', 64)";
    std::vector<long long> expected_pks;
    std::size_t row_count = 0;
    std::size_t sequence_length = 0;
};

/** @brief Correctness and timing stats from one SQL sort scan. */
struct sort_scan_output {
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

[[nodiscard]] run_constraints sort_scan_estimate(
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

[[nodiscard]] sort_scan_input build_sort_scan_input(
    std::string_view,
    std::string_view test_case_name,
    simdjson::dom::element data,
    const run_constraints&)
{
    sort_scan_input in;
    in.conn = std::make_shared<it::connection_test_context>();
    in.table = "bench_sort_scan_" + sort_scan_identifier(test_case_name);
    in.row_count = static_cast<std::size_t>(jint(data, "row_count", 65536));
    in.sequence_length = static_cast<std::size_t>(jint(data, "sequence_length", 32));
    const std::size_t prefix_len = static_cast<std::size_t>(jint(data, "prefix_length", 0));
    const std::size_t key_digits = static_cast<std::size_t>(jint(data, "key_digits", 32));
    const std::int64_t multiplier = jint(data, "permutation_multiplier", 32771);
    if (in.sequence_length < prefix_len + key_digits) {
        bioseq_log<logging_level::error>(
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
            bioseq_log<logging_level::error>(
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
    const sort_scan_input& in)
{
    const char* keep_artifacts_env = std::getenv("BIOSEQ_E2E_KEEP_ARTIFACTS");
    if (keep_artifacts_env != nullptr && std::string_view{keep_artifacts_env} == "1") {
        return;
    }
    if (in.conn != nullptr) {
        it::exec_sqlf(*in.conn, "DROP TABLE IF EXISTS {} CASCADE;", in.table);
    }
}

[[nodiscard]] sort_scan_output sort_scan_run(
    std::string_view algo,
    const parameter_map& params,
    const sort_scan_input& in)
{
    sort_scan_output out;
    out.row_count = static_cast<std::int64_t>(in.row_count);
    const std::string implementation = param_string(params, "implementation", "gpu");
    const bool use_gpu = implementation == "gpu" || algo == "sort_scan_gpu";
    it::exec_sql(*in.conn, "SET bioseq.planner.sequence_sort_scan.enabled = on;");
    it::exec_sqlf(*in.conn, "SET bioseq.aligner.vulkan.enabled = {};",
                  use_gpu ? "on" : "off");

    const std::string sql = fmt::format("SELECT pk FROM {} ORDER BY seq0;", in.table);
    scoped_resource_monitor resources;
    const auto start_ns = static_cast<std::uint64_t>(bioseq::instrumentation::clock::now_ns());
    std::vector<long long> pks;
    {
        auto _scope = bioseq::instrumentation::region(k_measured_region);
        pks = it::query_int_column(*in.conn, sql);
    }
    const auto end_ns = static_cast<std::uint64_t>(bioseq::instrumentation::clock::now_ns());
    (void)resources.stop();
    out.measured_region_ns = end_ns - start_ns;
    out.sorted_correct = (pks == in.expected_pks) ? 1 : 0;
    if (out.sorted_correct == 0) {
        bioseq_log<logging_level::error>(
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
    const parameter_map&,
    const sort_scan_input&,
    const sort_scan_output& out)
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

#if defined(BIOSEQ_E2E_TESTING_API)
/**
 * @brief Exact production contracts exposed only while compiling the E2E test
 * target.
 *
 * The public benchmark module remains launcher-only.  This conditional bridge
 * lets the separately owned `bioseq.bench.e2e.tests.support` module exercise
 * the production serializer, evaluator, setup resolver, and parsers without
 * textually including a translation unit or publishing them to production
 * consumers.
 */
export namespace bioseq::bench::e2e::detail {

using config_value = bioseq::bench::framework::config_value;
using parameter_map = bioseq::bench::framework::parameter_map;
using resolved_execution_tuple =
    bioseq::bench::framework::resolved_execution_tuple;
using rowset = bioseq_int_tests::rowset_t;
using run_constraints = bioseq::bench::framework::run_constraints;

[[nodiscard]] std::array<std::string, 22>
algorithm_matrix_session_sql(
        int kmer_size,
        int window_size,
        std::string_view minimizer,
        std::string_view chaining,
        std::string_view alignment,
        std::string_view pipeline_mode,
        std::string_view minimizer_index,
        bool build_target) {
    return render_bioseq_algorithm_matrix_session_sql(
        kmer_size,
        window_size,
        minimizer,
        chaining,
        alignment,
        pipeline_mode,
        minimizer_index,
        build_target);
}

[[nodiscard]] std::span<const std::string_view>
cold_index_stats_keys() noexcept {
    return k_cold_index_stats_keys;
}

[[nodiscard]] std::span<const std::string_view>
minimizer_fragment_reader_stats_keys() noexcept {
    return k_minimizer_fragment_reader_stats_keys;
}

[[nodiscard]] std::span<const std::string_view>
persisted_gpu_operation_keys() noexcept {
    return k_persisted_gpu_operation_keys;
}

[[nodiscard]] std::string_view minimizers_per_fragment_index() noexcept {
    return k_minimizers_per_fragment_index;
}

[[nodiscard]] bool ordinary_algorithm_uses_iteration_boundaries(
        const std::string_view algorithm,
        const bool paired_minimap2_oracle,
        const bool algorithm_matrix_measurement_scope = true) noexcept {
    e2e_input input;
    input.paired_minimap2_oracle = paired_minimap2_oracle;
    input.algorithm_matrix_measurement_scope =
        algorithm_matrix_measurement_scope;
    return use_e2e_iteration_boundaries(algorithm, input);
}

[[nodiscard]] std::string serialize_raw_sample(
        std::string_view algorithm,
        const parameter_map& parameters,
        const e2e_input& input,
        const e2e_output& output) {
    return serialize_e2e_raw_sample(
        algorithm, parameters, input, output);
}

[[nodiscard]] std::map<std::string, double> evaluate(
        std::string_view algorithm,
        const parameter_map& parameters,
        const e2e_input& input,
        const e2e_output& output) {
    return e2e_eval(algorithm, parameters, input, output);
}

[[nodiscard]] e2e_just_index_setup_requirements
resolve_just_index_setup_requirements(
        const run_constraints& constraints) {
    return resolve_e2e_just_index_setup_requirements(constraints);
}

[[nodiscard]] bool persisted_setup_must_reject(const e2e_input& input,
                                               std::string_view minimizer,
                                               std::string_view minimizer_index) noexcept {
    return algorithm_matrix_persisted_setup_must_reject(input, minimizer, minimizer_index);
}

[[nodiscard]] bool is_complete_vulkan_algorithm_matrix_tuple(
        const e2e_input& input,
        const e2e_output& output) noexcept {
    return ::is_complete_vulkan_algorithm_matrix_tuple(input, output);
}

[[nodiscard]] std::string projection_sql(const e2e_input& input) {
    return e2e_projection_sql(input);
}

[[nodiscard]] std::string batched_transformed_coordinate_sql(
        const std::string& target_table,
        std::string_view projection,
        std::int64_t query_start,
        std::int64_t target_start) {
    return batched_transformed_coord_sql(
        target_table, projection, query_start, target_start);
}

void parse_bioseq_rows(
        const rowset& rows,
        const e2e_input& input,
        e2e_output& output) {
    initialize_ordered_query_oracle(input, output);
    parse_bioseq_batch_rows(rows, input, output);
    finalize_ordered_query_oracle(input, output);
}

void parse_minimap2_rows(
        const std::string& paf,
        const e2e_input& input,
        e2e_output& output) {
    initialize_ordered_query_oracle(input, output);
    parse_minimap2_paf(paf, input, output);
    finalize_ordered_query_oracle(input, output);
}

void finalize_paired_oracle_repeat(
        const e2e_input& input,
        const std::size_t repeat,
        e2e_output& bioseq_output,
        e2e_output& minimap2_output) {
    parameter_map parameters;
    std::array<bioseq::bench::framework::repeat_output_reference<e2e_output>, 2>
        outputs{{
            {
                .algorithm_name =
                    bioseq::bench::e2e::detail::algorithm_matrix_just_index,
                .params = &parameters,
                .output = &bioseq_output,
            },
            {
                .algorithm_name = "minimap2_just_index",
                .params = &parameters,
                .output = &minimap2_output,
            },
        }};
    finalize_paired_minimap2_oracle_repeat(input, repeat, outputs);
}

void annotate_behavior_differences(
        const e2e_input& input,
        e2e_output& output) {
    annotate_accepted_behavior_differences(input, output);
}

}  // namespace bioseq::bench::e2e::detail
#endif

namespace {

struct e2e_config_paths {
    std::string identity_path;
    std::string open_path;
};

[[nodiscard]] bool malformed_e2e_config_path_text(
        const std::string_view value) noexcept {
    if (value.empty()
        || static_cast<unsigned char>(value.front()) <= 0x20
        || static_cast<unsigned char>(value.back()) <= 0x20) {
        return true;
    }
    return std::ranges::any_of(value, [](const char character) {
        const auto byte = static_cast<unsigned char>(character);
        return byte < 0x20 || byte == 0x7f;
    });
}

[[nodiscard]] bool exact_regular_absolute_file(
        const std::filesystem::path& path) noexcept {
    if (!path.is_absolute()
        || path.lexically_normal().generic_string() != path.generic_string()
        || path.extension() != ".json") {
        return false;
    }
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error || !std::filesystem::is_regular_file(status)
        || std::filesystem::is_symlink(status)) {
        return false;
    }
    const auto canonical = std::filesystem::canonical(path, error);
    return !error && canonical == path;
}

[[nodiscard]] bool canonical_relative_e2e_config_identity(
        const std::string_view identity,
        const std::filesystem::path& path) noexcept {
    if (malformed_e2e_config_path_text(identity) || path.is_absolute()
        || identity.contains('\\')
        || path.lexically_normal().generic_string() != identity) {
        return false;
    }
    return std::ranges::none_of(path, [](const auto& component) {
        return component.empty() || component == "." || component == "..";
    });
}

[[nodiscard]] bool canonical_q1_calibration_e2e_config_identity(
        const std::string_view identity) noexcept {
    constexpr std::string_view q1_directory =
        "bench/e2e_algorithm_matrix_q1_calibration_shards/";
    if (!identity.starts_with(q1_directory)) return false;
    try {
        const std::filesystem::path manifest_path =
            std::filesystem::path{BIOSEQ_E2E_DATA_DIR}
            / "bench_e2e_algorithm_matrix_q1_calibration_manifest.json";
        if (!exact_regular_absolute_file(manifest_path)) return false;
        std::ifstream stream{manifest_path};
        if (!stream.good()) return false;
        const nlohmann::json manifest = nlohmann::json::parse(stream);
        if (!manifest.is_object()
            || manifest.value("schema", std::string{})
                != "bioseq.e2e_algorithm_matrix_q1_calibration_manifest"
            || manifest.value("schema_version", 0) != 2
            || manifest.value("identity", std::string{})
                != "Q1_CALIBRATION_PANEL"
            || manifest.value("status", std::string{})
                != "local_only_static_configuration"
            || !manifest.contains("panel_contract")
            || !manifest.at("panel_contract").is_object()
            || manifest.at("panel_contract").value("result_cells", 0) != 5
            || manifest.at("panel_contract").value(
                "repetitions_per_cell", 0) != 5
            || manifest.at("panel_contract").value(
                "expected_raw_records", 0) != 25
            || !manifest.contains("nonpublication_rule")
            || !manifest.at("nonpublication_rule").is_object()
            || manifest.at("nonpublication_rule").value(
                "merge_authority", std::string{}) != "none"
            || !manifest.contains("cells")
            || !manifest.at("cells").is_array()
            || manifest.at("cells").size() != 5) {
            return false;
        }
        std::set<std::string> unique_paths;
        std::size_t matches = 0;
        std::size_t ordinal = 0;
        for (const nlohmann::json& cell : manifest.at("cells")) {
            if (!cell.is_object()
                || cell.value("ordinal", std::numeric_limits<std::size_t>::max())
                    != ordinal++
                || cell.value("test_case", std::string{})
                    != "M2_D4_K15_W10_Q1_CALIBRATION"
                || cell.value("repetitions", 0) != 5
                || cell.value("expected_records", 0) != 5
                || !cell.contains("path") || !cell.at("path").is_string()
                || !cell.contains("config_sha256")
                || !cell.at("config_sha256").is_string()) {
                return false;
            }
            const std::string path_text = cell.at("path").get<std::string>();
            const std::string digest =
                cell.at("config_sha256").get<std::string>();
            const std::filesystem::path relative_path{path_text};
            if (!path_text.starts_with(q1_directory)
                || !canonical_relative_e2e_config_identity(
                    path_text, relative_path)
                || digest.size() != 64
                || !std::ranges::all_of(digest, [](const char character) {
                    return (character >= '0' && character <= '9')
                        || (character >= 'a' && character <= 'f');
                })
                || !unique_paths.insert(path_text).second) {
                return false;
            }
            if (path_text == identity) ++matches;
        }
        return unique_paths.size() == 5 && matches == 1;
    } catch (...) {
        return false;
    }
}

[[nodiscard]] bool canonical_deadline_campaign_e2e_config_identity(
        const std::string_view identity) noexcept {
    constexpr std::string_view directory =
        "bench/e2e_algorithm_matrix_deadline_shards/";
    constexpr std::string_view campaign_digest =
        "3d03b3aea29a5418721ba5899c58a698f568ca10dddf9f00eaab42adf450a3b0";
    if (!identity.starts_with(directory)) return false;
    try {
        const std::filesystem::path manifest_path =
            std::filesystem::path{BIOSEQ_E2E_DATA_DIR}
            / "bench_e2e_algorithm_matrix_deadline_manifest.json";
        if (!exact_regular_absolute_file(manifest_path)) return false;
        std::ifstream stream{manifest_path};
        if (!stream.good()) return false;
        const nlohmann::json manifest = nlohmann::json::parse(stream);
        if (!manifest.is_object()
            || manifest.value("schema", std::string{})
                != "bioseq.e2e.algorithm-matrix-deadline-campaign"
            || manifest.value("version", 0) != 1
            || !manifest.contains("campaign")
            || !manifest.at("campaign").is_object()
            || manifest.at("campaign").value("campaign_id", std::string{})
                != "bioseq-e2e-algorithm-matrix-deadline-v1"
            || manifest.at("campaign").value(
                "campaign_digest", std::string{}) != campaign_digest
            || manifest.at("campaign").value("q1_membership", -1) != 0
            || manifest.at("campaign").value("expected_shards", 0) != 59
            || manifest.at("campaign").value("expected_raw_records", 0) != 295
            || !manifest.contains("runtime_contract")
            || !manifest.at("runtime_contract").is_object()
            || manifest.at("runtime_contract").value(
                "prefilter_enabled", true)
            || !manifest.at("runtime_contract").value(
                "ksw2_batched_excluded", false)
            || manifest.at("runtime_contract").value(
                "extended_timeout_shard_count", 0) != 1
            || !manifest.contains("publication_contract")
            || !manifest.at("publication_contract").is_object()
            || manifest.at("publication_contract").value(
                "merge_into_ALL_RESULTS_yaml", std::string{})
                != "eligible_after_terminal_per_shard_audit"
            || manifest.at("publication_contract").value(
                "merge_into_Q8_RESULTS_yaml", true)
            || !manifest.contains("schedule")
            || !manifest.at("schedule").is_array()
            || manifest.at("schedule").size() != 59) {
            return false;
        }
        std::set<std::string> unique_paths;
        std::set<std::string> unique_hashes;
        std::size_t matches = 0;
        std::size_t extended = 0;
        std::size_t ordinal = 0;
        for (const nlohmann::json& shard : manifest.at("schedule")) {
            if (!shard.is_object()
                || shard.value("ordinal", std::numeric_limits<std::size_t>::max())
                    != ordinal++
                || shard.value("repetitions", 0) != 5
                || shard.value("expected_records", 0) != 5
                || !shard.contains("path") || !shard.at("path").is_string()
                || !shard.contains("config_sha256")
                || !shard.at("config_sha256").is_string()) {
                return false;
            }
            const std::string path_text = shard.at("path").get<std::string>();
            const std::string digest =
                shard.at("config_sha256").get<std::string>();
            const std::filesystem::path relative_path{path_text};
            if (!path_text.starts_with(directory)
                || !canonical_relative_e2e_config_identity(
                    path_text, relative_path)
                || digest.size() != 64
                || !std::ranges::all_of(digest, [](const char character) {
                    return (character >= '0' && character <= '9')
                        || (character >= 'a' && character <= 'f');
                })
                || !unique_paths.insert(path_text).second
                || !unique_hashes.insert(digest).second) {
                return false;
            }
            if (shard.value("runtime_timeout_seconds", 0) == 172800) {
                ++extended;
            }
            if (path_text == identity) ++matches;
        }
        return unique_paths.size() == 59 && unique_hashes.size() == 59
            && extended == 1 && matches == 1;
    } catch (...) {
        return false;
    }
}

[[nodiscard]] bool canonical_deadline_campaign_v2_e2e_config_identity(
        const std::string_view identity) noexcept {
    constexpr std::string_view directory =
        "bench/e2e_algorithm_matrix_deadline_v2_shards/";
    constexpr std::string_view campaign_digest =
            "44a85360a5e975ec8a6d70ac0b06486c51d748770c2ada46106d2ccc5db4728a";
    constexpr std::string_view parent_campaign_digest =
        "3d03b3aea29a5418721ba5899c58a698f568ca10dddf9f00eaab42adf450a3b0";
    constexpr std::string_view parent_manifest_sha256 =
        "73cd6928e7430c46bff2836f0bb0820565e9d5c9b0011939ff4aad9fbe771fa0";
    constexpr std::string_view parent_shard_tree_sha256 =
        "297634d7b22f4ca05bef640282acd09c92e17cd23b544ce624e1c59ba4838270";
    if (!identity.starts_with(directory)) return false;
    try {
        const std::filesystem::path manifest_path =
            std::filesystem::path{BIOSEQ_E2E_DATA_DIR}
            / "bench_e2e_algorithm_matrix_deadline_v2_manifest.json";
        if (!exact_regular_absolute_file(manifest_path)) return false;
        std::ifstream stream{manifest_path};
        if (!stream.good()) return false;
        const nlohmann::json manifest = nlohmann::json::parse(stream);
        if (!manifest.is_object()
            || manifest.value("schema", std::string{})
                != "bioseq.e2e.algorithm-matrix-deadline-campaign"
            || manifest.value("version", 0) != 2
            || !manifest.contains("lineage")
            || !manifest.at("lineage").is_object()
            || manifest.at("lineage").value(
                "parent_campaign_id", std::string{})
                != "bioseq-e2e-algorithm-matrix-deadline-v1"
            || manifest.at("lineage").value(
                "parent_campaign_digest", std::string{})
                != parent_campaign_digest
            || manifest.at("lineage").value(
                "parent_manifest", std::string{})
                != "bench/bench_e2e_algorithm_matrix_deadline_manifest.json"
            || manifest.at("lineage").value(
                "parent_manifest_sha256", std::string{})
                != parent_manifest_sha256
            || manifest.at("lineage").value(
                "parent_shard_directory", std::string{})
                != "bench/e2e_algorithm_matrix_deadline_shards"
            || manifest.at("lineage").value(
                "parent_shard_tree_sha256", std::string{})
                != parent_shard_tree_sha256
            || manifest.at("lineage").value("parent_expected_shards", 0) != 59
            || manifest.at("lineage").value(
                "parent_expected_raw_records", 0) != 295
            || manifest.at("lineage").value(
                "parent_artifact_policy", std::string{})
                != "immutable_exact_parent_authority"
            || !manifest.contains("campaign")
            || !manifest.at("campaign").is_object()
            || manifest.at("campaign").value("campaign_id", std::string{})
                != "bioseq-e2e-algorithm-matrix-deadline-v2"
            || manifest.at("campaign").value(
                "campaign_digest", std::string{}) != campaign_digest
            || manifest.at("campaign").value("q1_membership", -1) != 0
            || manifest.at("campaign").value("repetitions_per_cell", 0) != 5
            || manifest.at("campaign").value("expected_shards", 0) != 60
            || manifest.at("campaign").value("expected_result_cells", 0) != 60
            || manifest.at("campaign").value("expected_raw_records", 0) != 300
            || manifest.at("campaign").value(
                "estimated_wall_seconds", 0) != 586467
            || manifest.at("campaign").value("deadline_seconds", 0) != 604800
            || manifest.at("campaign").value(
                "operational_margin_seconds", 0) != 18333
            || !manifest.contains("runtime_contract")
            || !manifest.at("runtime_contract").is_object()
            || manifest.at("runtime_contract").value(
                "prefilter_enabled", true)
            || !manifest.at("runtime_contract").value(
                "cigar_projection", false)
            || !manifest.at("runtime_contract").value(
                "mapq_projection", false)
            || !manifest.at("runtime_contract").value(
                "ksw2_batched_excluded", false)
            || manifest.at("runtime_contract").value(
                "extended_timeout_shard_count", 0) != 2
            || !manifest.contains("publication_contract")
            || !manifest.at("publication_contract").is_object()
            || manifest.at("publication_contract").value(
                "merge_into_ALL_RESULTS_yaml", std::string{})
                != "eligible_after_terminal_per_shard_audit"
            || manifest.at("publication_contract").value(
                "automatic_merge_during_run_only", true)
            || manifest.at("publication_contract").value(
                "merge_into_Q8_RESULTS_yaml", true)
            || !manifest.contains("schedule")
            || !manifest.at("schedule").is_array()) {
            return false;
        }
        const std::size_t expected_shards = manifest.at("campaign").value(
            "expected_shards", std::numeric_limits<std::size_t>::max());
        const std::size_t repetitions = manifest.at("campaign").value(
            "repetitions_per_cell", std::numeric_limits<std::size_t>::max());
        if (expected_shards != 60 || repetitions != 5
            || manifest.at("campaign").value(
                "expected_result_cells", std::size_t{}) != expected_shards
            || manifest.at("campaign").value(
                "expected_raw_records", std::size_t{})
                != expected_shards * repetitions
            || manifest.at("schedule").size() != expected_shards) {
            return false;
        }
        std::set<std::string> unique_paths;
        std::set<std::string> unique_hashes;
        std::set<std::string> unique_stable_keys;
        std::size_t matches = 0;
        std::size_t extended = 0;
        std::size_t ordinal = 0;
        bool persisted_q1024 = false;
        bool no_index_q1024 = false;
        for (const nlohmann::json& shard : manifest.at("schedule")) {
            if (!shard.is_object()
                || shard.value("ordinal", std::numeric_limits<std::size_t>::max())
                    != ordinal++
                || shard.value("repetitions", 0) != repetitions
                || shard.value("expected_records", 0) != repetitions
                || !shard.contains("path") || !shard.at("path").is_string()
                || !shard.contains("config_sha256")
                || !shard.at("config_sha256").is_string()
                || !shard.contains("stable_key")
                || !shard.at("stable_key").is_string()
                || !shard.contains("identity")
                || !shard.at("identity").is_object()) {
                return false;
            }
            const std::string path_text = shard.at("path").get<std::string>();
            const std::string digest =
                shard.at("config_sha256").get<std::string>();
            const std::string stable_key =
                shard.at("stable_key").get<std::string>();
            const std::filesystem::path relative_path{path_text};
            if (!path_text.starts_with(directory)
                || !canonical_relative_e2e_config_identity(
                    path_text, relative_path)
                || digest.size() != 64
                || !std::ranges::all_of(digest, [](const char character) {
                    return (character >= '0' && character <= '9')
                        || (character >= 'a' && character <= 'f');
                })
                || !stable_key.starts_with("bioseq-e2e-deadline-cell-v2:")
                || !unique_paths.insert(path_text).second
                || !unique_hashes.insert(digest).second
                || !unique_stable_keys.insert(stable_key).second) {
                return false;
            }
            const auto& shard_identity = shard.at("identity");
            if (shard_identity.value("campaign_id", std::string{})
                    != "bioseq-e2e-algorithm-matrix-deadline-v2") {
                return false;
            }
            if (shard.value("runtime_timeout_seconds", 0) == 172800) {
                ++extended;
                const bool common_q1024_identity =
                    shard_identity.value("tier", std::string{}) == "q1024_anchor"
                    && shard_identity.value("query_count", 0) == 1024
                    && shard_identity.value("dataset", std::string{})
                        == "M2_D4_K15_W10"
                    && shard_identity.value("engine", std::string{}) == "bioseq"
                    && shard_identity.value("bioseq_index_name", std::string{})
                        == "minimizers_per_fragment"
                    && shard_identity.value(
                        "requested_minimizer_algorithm", std::string{}) == "scalar"
                    && shard_identity.value(
                        "requested_chaining_algorithm", std::string{})
                        == "scalar_minimap2"
                    && shard_identity.value(
                        "requested_base_alignment_algorithm", std::string{})
                        == "ksw2";
                if (!common_q1024_identity) return false;
                if (shard.value("ordinal", 0) == 58
                    && shard_identity.value("index_mode", std::string{})
                        == "just_index") {
                    persisted_q1024 = true;
                } else if (shard.value("ordinal", 0) == 59
                    && shard_identity.value("index_mode", std::string{})
                        == "no_index") {
                    no_index_q1024 = true;
                } else {
                    return false;
                }
            }
            if (path_text == identity) ++matches;
        }
        return unique_paths.size() == expected_shards
            && unique_hashes.size() == expected_shards
            && unique_stable_keys.size() == expected_shards
            && extended == manifest.at("runtime_contract").value(
                "extended_timeout_shard_count", std::size_t{})
            && persisted_q1024 && no_index_q1024 && matches == 1;
    } catch (...) {
        return false;
    }
}

struct sealed_e2e_config_identity {
    std::string_view path;
    std::string_view sha256;
};

constexpr std::array<sealed_e2e_config_identity, 19>
    k_post_s059_seven_day_queue_configs{{
        {
            "bench/e2e_algorithm_matrix_q1024_packed_shards/"
            "bench_e2e_algorithm_matrix_q1024_packed_m2_d4_k20_w15_"
            "minimap2_both_modes.json",
            "5c9d6f8586f73298f3b8e229e44d5079f8a5621a6267cbd234363aedb803eb00",
        },
        {
            "bench/e2e_algorithm_matrix_q1024_packed_shards/"
            "bench_e2e_algorithm_matrix_q1024_packed_m2_d5_k15_w10_"
            "minimap2_both_modes.json",
            "089f6c629207950434842820ffdb25953264277b747b0f6731e5fcbdcf34bba9",
        },
        {
            "bench/e2e_algorithm_matrix_q1024_packed_shards/"
            "bench_e2e_algorithm_matrix_q1024_packed_m2_d5_k20_w15_"
            "minimap2_both_modes.json",
            "174192e25a5cd6d587afab373dcce0f73ad3262dbbb1fa5c763c8b8b2ae35f8d",
        },
        {
            "bench/e2e_algorithm_matrix_q1024_packed_shards/"
            "bench_e2e_algorithm_matrix_q1024_packed_m4_d4_k15_w10_"
            "minimap2_both_modes.json",
            "18fc890771a6c0fbd758cebd61cbda2213ff14fa51311e8fc6f3d547907c8211",
        },
        {
            "bench/e2e_algorithm_matrix_q1024_packed_shards/"
            "bench_e2e_algorithm_matrix_q1024_packed_m4_d4_k20_w15_"
            "minimap2_both_modes.json",
            "d5eebe28de4dac44f73c51f96abaee48763fca53ed143921aa12a5035b76a4c4",
        },
        {
            "bench/e2e_algorithm_matrix_q1024_packed_shards/"
            "bench_e2e_algorithm_matrix_q1024_packed_m4_d5_k15_w10_"
            "minimap2_both_modes.json",
            "89f7538571e0bfa6303eeb6f982165a0691bbee8b2db6a8f38282b65d090b875",
        },
        {
            "bench/e2e_algorithm_matrix_q1024_packed_shards/"
            "bench_e2e_algorithm_matrix_q1024_packed_m4_d5_k20_w15_"
            "minimap2_both_modes.json",
            "2780eab9ea0707467115b523b0abf5477c34e81f8a7dc48eef43217d2a19f183",
        },
        {
            "bench/e2e_algorithm_matrix_shards/"
            "bench_e2e_algorithm_matrix_campaign_shard_singleton_0660_"
            "92411d3a09458e94.json",
            "9ca0ecbc5c83bef850cd7e2fdd2643f6387fb88edaaaf0aa49e772177f247218",
        },
        {
            "bench/e2e_algorithm_matrix_shards/"
            "bench_e2e_algorithm_matrix_campaign_shard_m2_d4_k15_w10_"
            "just_index_bounded_nthash_simd_mpfragment.json",
            "71b1974fb676f64da910e6590c63229db0aa8369965622f11c61342837bf6069",
        },
        {
            "bench/e2e_algorithm_matrix_shards/"
            "bench_e2e_algorithm_matrix_campaign_shard_m2_d4_k15_w10_"
            "just_index_simd_minimap2_reference_mpfragment.json",
            "bb637ae265c7f57ce1b9fb24c8b85cfba56c256c85505d808de16c2a1610010e",
        },
        {
            "bench/e2e_algorithm_matrix_shards/"
            "bench_e2e_algorithm_matrix_campaign_shard_m2_d4_k15_w10_"
            "just_index_two_stack_nthash_mpfragment.json",
            "a205e40e49a19448abf7539a2ed28251e7b0f225793f0b6dbe2ed1df49ef8484",
        },
        {
            "bench/e2e_algorithm_matrix_q1024_packed_shards/"
            "bench_e2e_algorithm_matrix_q1024_packed_m2_d4_k15_w10_"
            "just_index_scalar_mpfragment_residual_after_s058.json",
            "0a992c635e053530bf53257b7cf5e9d22cf61079ab71d71aa375fe954889b330",
        },
        {
            "bench/e2e_algorithm_matrix_shards/"
            "bench_e2e_algorithm_matrix_campaign_shard_m4_d4_k15_w10_"
            "just_index_scalar_mpfragment.json",
            "a39fda9baa701ba622e7ef4f4d60506f7ff4a59911f0ddb73ee21efa45fd6c64",
        },
        {
            "bench/e2e_algorithm_matrix_shards/"
            "bench_e2e_algorithm_matrix_campaign_shard_m4_d4_k15_w10_"
            "just_index_bounded_nthash_simd_mpfragment.json",
            "18fc0f1f8b85bbdc774d848f6b72b7ca2145a55177eaa371340f30bb8d06ccc4",
        },
        {
            "bench/e2e_algorithm_matrix_shards/"
            "bench_e2e_algorithm_matrix_campaign_shard_m4_d4_k15_w10_"
            "just_index_simd_minimap2_reference_mpfragment.json",
            "ff9dfd6a359767c8268f0af6329b8df55ca26715622330bb0588eb467c582372",
        },
        {
            "bench/e2e_algorithm_matrix_shards/"
            "bench_e2e_algorithm_matrix_campaign_shard_m4_d4_k15_w10_"
            "just_index_two_stack_nthash_mpfragment.json",
            "69ae99a86696bdcdf177e97d36a745bd658f6a05641999f634b29f08b7a0f810",
        },
        {
            "bench/e2e_algorithm_matrix_shards/"
            "bench_e2e_algorithm_matrix_campaign_shard_singleton_0150_"
            "c9d4c0fd1968374d.json",
            "3f1ea122dbf2aed74674fbf1ef28b2461e21a87e98036fdb7053dba8c5be8f32",
        },
        {
            "bench/e2e_algorithm_matrix_shards/"
            "bench_e2e_algorithm_matrix_campaign_shard_singleton_0068_"
            "2496323c88e8add2.json",
            "7cbedf6a5ccc42f304e37b6a1faffbc4c3e19d6fb592c237ecdc3a141ad14b44",
        },
        {
            "bench/e2e_algorithm_matrix_q1024_packed_shards/"
            "bench_e2e_algorithm_matrix_q1024_packed_m2_d4_k20_w15_"
            "just_index_scalar_mpfragment_block_aligner_residual_after_0739.json",
            "37765d4a16781ac9e54571ce9f8d55ea5113fa742192f25a3e243f3b21ba325f",
        },
    }};

[[nodiscard]] bool canonical_post_s059_queue_e2e_config_identity(
        const std::string_view identity) noexcept {
    return std::ranges::any_of(
        k_post_s059_seven_day_queue_configs,
        [identity](const sealed_e2e_config_identity& config) {
            return config.path == identity
                && config.sha256.size() == 64
                && std::ranges::all_of(
                    config.sha256,
                    [](const char character) {
                        return (character >= '0' && character <= '9')
                            || (character >= 'a' && character <= 'f');
                    });
        });
}

[[nodiscard]] bool canonical_campaign_e2e_config_identity(
        const std::string_view identity) noexcept {
    // The worker validates exact manifest membership and bytes before setting
    // the dual-path environment. Q1 has a closed five-member manifest rather
    // than a shard-name family, so derive its membership from that seal.
    if (canonical_post_s059_queue_e2e_config_identity(identity)) return true;
    if (canonical_deadline_campaign_v2_e2e_config_identity(identity)) return true;
    if (canonical_deadline_campaign_e2e_config_identity(identity)) return true;
    if (canonical_q1_calibration_e2e_config_identity(identity)) return true;
    constexpr std::array campaign_identities{
        std::pair<std::string_view, std::string_view>{
            "bench/bench_e2e_algorithm_matrix_campaign.json",
            "bench/e2e_algorithm_matrix_shards/"
            "bench_e2e_algorithm_matrix_campaign_shard_"},
        std::pair<std::string_view, std::string_view>{
            "bench/bench_e2e_algorithm_matrix_q8_campaign.json",
            "bench/e2e_algorithm_matrix_q8_shards/"
            "bench_e2e_algorithm_matrix_q8_campaign_shard_"},
    };
    return std::ranges::any_of(
        campaign_identities,
        [identity](const auto& campaign) {
            const auto& [monolith, shard_prefix] = campaign;
            constexpr std::string_view json_suffix = ".json";
            if (identity == monolith) return true;
            if (!identity.starts_with(shard_prefix)
                || !identity.ends_with(json_suffix)
                || identity.size()
                    <= shard_prefix.size() + json_suffix.size()) {
                return false;
            }
            const auto shard_name = identity.substr(
                shard_prefix.size(),
                identity.size() - shard_prefix.size() - json_suffix.size());
            return std::ranges::all_of(
                shard_name,
                [](const char character) {
                    return character == '_'
                        || (character >= 'a' && character <= 'z')
                        || (character >= '0' && character <= '9');
                });
        });
}

[[nodiscard]] std::optional<e2e_config_paths>
validated_e2e_config_paths_from_environment() noexcept {
    const char* identity_environment =
        std::getenv("BIOSEQ_E2E_CONFIG_OVERRIDE");
    const char* open_path_environment =
        std::getenv("BIOSEQ_E2E_CONFIG_OPEN_PATH");

    // Direct/manual launches that do not opt into the worker's dual-path
    // contract retain the historical single verbatim config path.
    if (open_path_environment == nullptr) {
        const std::string identity{
            identity_environment != nullptr && *identity_environment != '\0'
                ? identity_environment
                : BIOSEQ_E2E_CONFIG_PATH};
        return e2e_config_paths{identity, identity};
    }
    const auto reject = [](const std::string_view reason) {
        bioseq_log<logging_level::error>(
            "[bench_e2e] rejected E2E config identity/open-path contract: {}",
            reason);
        return std::optional<e2e_config_paths>{};
    };
    if (identity_environment == nullptr || *identity_environment == '\0') {
        return reject("open path appeared without a primary override");
    }

    const std::string identity{identity_environment};
    const std::string open_path_text{open_path_environment};
    if (malformed_e2e_config_path_text(identity)
        || malformed_e2e_config_path_text(open_path_text)) {
        return reject("identity or open path is malformed");
    }
    const std::filesystem::path open_path{open_path_text};
    if (!exact_regular_absolute_file(open_path)) {
        return reject(
            "open path is not one exact canonical absolute regular JSON file");
    }

    const std::filesystem::path identity_path{identity};
    std::filesystem::path expected_path;
    if (identity_path.is_absolute()) {
        if (!exact_regular_absolute_file(identity_path)
            || identity_path.generic_string() != open_path.generic_string()) {
            return reject(
                "absolute identity and open path are not the same exact file path");
        }
        expected_path = identity_path;
    } else {
        if (!canonical_relative_e2e_config_identity(identity, identity_path)
            || !canonical_campaign_e2e_config_identity(identity)) {
            return reject(
                "relative identity is not one canonical campaign config");
        }
        const std::filesystem::path benchmark_root{BIOSEQ_E2E_DATA_DIR};
        if (!benchmark_root.is_absolute()) {
            return reject("compiled benchmark root is not absolute");
        }
        expected_path = benchmark_root.parent_path() / identity_path;
        if (!exact_regular_absolute_file(expected_path)
            || expected_path.generic_string() != open_path.generic_string()) {
            return reject(
                "relative identity does not name the supplied repository file");
        }
    }

    std::error_code error;
    const bool same_file = std::filesystem::equivalent(
        expected_path, open_path, error);
    if (error || !same_file) {
        return reject("identity and open path alias different file content");
    }
    return e2e_config_paths{identity, open_path_text};
}

}  // namespace

#if defined(BIOSEQ_E2E_TESTING_API)
export namespace bioseq::bench::e2e::detail {

/** @brief Test-only view of the worker-aligned campaign identity gate. */
[[nodiscard]] bool is_canonical_campaign_e2e_config_identity(
        const std::string_view identity) noexcept {
    return canonical_campaign_e2e_config_identity(identity);
}

}  // namespace bioseq::bench::e2e::detail
#endif

export namespace bioseq::bench::e2e {

int run(int argc, char** argv) {
    configure_logging(true);
    bioseq_set_fallback_debug_logging_enabled(false);
    const auto cfg_paths = validated_e2e_config_paths_from_environment();
    if (!cfg_paths.has_value()) return 1;
    const std::string& cfg = cfg_paths->identity_path;
    const std::string& cfg_open = cfg_paths->open_path;
    const std::string rep{BIOSEQ_E2E_REPORT_DIR}, data{BIOSEQ_E2E_DATA_DIR};
    using bioseq::bench::framework::run_constraints; using bioseq::bench::framework::parameter_map;
    auto est = std::function<run_constraints(std::string_view, std::string_view, simdjson::dom::element)>{e2e_estimate};
    auto inp = std::function<e2e_input(std::string_view, std::string_view, simdjson::dom::element, const run_constraints&)>{build_e2e_input};
    auto e2e_runner = std::function<e2e_output(std::string_view, const parameter_map&, const e2e_input&)>{e2e_run};
    auto evl = std::function<std::map<std::string,double>(std::string_view, const parameter_map&, const e2e_input&, const e2e_output&)>{e2e_eval};
    auto cln = std::function<void(std::string_view, std::string_view, simdjson::dom::element, const e2e_input&)>{cleanup_e2e_input};
    auto raw = std::function<std::string(std::string_view, const parameter_map&, const e2e_input&, const e2e_output&)>{serialize_e2e_raw_sample};
    bioseq::bench::framework::iteration_boundary_callbacks<
        e2e_input, e2e_iteration_preparation, e2e_output>
        iteration_boundaries{
            .enabled = [](
                    const std::string_view algorithm,
                    const parameter_map&,
                    const e2e_input& input) noexcept {
                return use_e2e_iteration_boundaries(algorithm, input);
            },
            .before_iteration = prepare_e2e_iteration,
            .measured_iteration = measure_e2e_iteration,
            .after_iteration = finalize_e2e_iteration,
        };
    bioseq::bench::framework::repeat_finalization_callbacks<
        e2e_input, e2e_output> paired_repeat_finalization{
        .enabled = [](const e2e_input& input) noexcept {
            return input.paired_minimap2_oracle;
        },
        .published_wall_clock_ns = [](
                std::string_view,
                const parameter_map&,
                const e2e_input&,
                const e2e_output& output) noexcept {
            return output.measured_region_available
                ? output.measured_region_ns : std::uint64_t{0};
        },
        .finalize_repeat = finalize_paired_minimap2_oracle_repeat,
    };
    auto sort_est = std::function<run_constraints(std::string_view, std::string_view, simdjson::dom::element)>{sort_scan_estimate};
    auto sort_inp = std::function<sort_scan_input(std::string_view, std::string_view, simdjson::dom::element, const run_constraints&)>{build_sort_scan_input};
    auto sort_run = std::function<sort_scan_output(std::string_view, const parameter_map&, const sort_scan_input&)>{sort_scan_run};
    auto sort_evl = std::function<std::map<std::string,double>(std::string_view, const parameter_map&, const sort_scan_input&, const sort_scan_output&)>{sort_scan_eval};
    auto sort_cln = std::function<void(std::string_view, std::string_view, simdjson::dom::element, const sort_scan_input&)>{cleanup_sort_scan_input};
    return bioseq::bench::framework::run(argc, argv, rep, data,
        bioseq::bench::framework::create_benchmark_with_iteration_boundaries<
            e2e_input, e2e_iteration_preparation, e2e_output>(
            "End-to-end SQL bioseq.load + mmsearch vs minimap2 (2/4 Gbp dna4/dna5)",
            "bench_e2e", cfg, est, inp, e2e_runner, evl,
            iteration_boundaries, cln, raw, cfg_open,
            paired_repeat_finalization),
        bioseq::bench::framework::create_benchmark<e2e_input, e2e_output>(
            "Five-repeat sign-off SQL bioseq.load + mmsearch vs minimap2 (2/4 Gbp dna4/dna5)",
            "bench_e2e_signoff", cfg, est, inp, e2e_runner, evl,
            cln, raw, cfg_open,
            paired_repeat_finalization),
        bioseq::bench::framework::create_benchmark<e2e_input, e2e_output>(
            "E2E resident GPU batch validation vs SIMD and minimap2",
            "bench_e2e_gpu_batch_validation", cfg, est, inp, e2e_runner, evl,
            cln, raw, cfg_open,
            paired_repeat_finalization),
        bioseq::bench::framework::create_benchmark<sort_scan_input, sort_scan_output>(
            "SQL ORDER BY bioseq.seq sort-scan GPU vs stdc_compare",
            "bench_sort_scan", cfg, sort_est, sort_inp, sort_run, sort_evl,
            sort_cln, {}, cfg_open));
}

}  // namespace bioseq::bench::e2e
