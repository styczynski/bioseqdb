/**
 * @file io_bench_v10_main.cpp
 * @brief IO bench entry point on top of bench_framework (V10).
 *
 * Replaces the legacy Catch2-based harness in `io/io_bench_main.cpp` and
 * its surrounding `io/io_bench/` tree. Benchmark blocks declared
 * in `io_bench_v10_configuration.json`:
 *
 *   * bench_io_readers_plain — POSIX / mmap / async-pipeline readers + kseqpp + naive_ifstream
 *   * bench_io_readers_gz    — parallel-gzip + zlib paths on the .fastq.gz cell
 *   * bench_io_writers       — sync / parallel4 / parallel8 bioseq writers + naive_ofstream
 *   * bench_io_lazy_sequence_expression — eager versus deferred slice/concat
 *
 * Each block sets `concurrency.bench_lock = true` and
 * `concurrency.concurrency = false` so cells run serially — IO paths
 * spawn many internal worker threads, and overlapping cells stalls the
 * box.
 */
#include "common.h"

#include <kseq++/seqio.hpp>
#include <simdjson.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <unistd.h>

import bioseq.common.mlogging;
import bioseq.common.sequan_compat;
import bioseq.sequences;
import bioseq.storage.lazy;
import bioseq.storage.size;
import bioseq.parallelism.memory_admission;
import bioseq.io.public_api;
import bioseq.io.options;
import bioseq.io.writer;
import bioseq.io.identifier_formats;   // DOC0295 §12.1 — header-parser overhead bench
import bioseq.io.identifier_parser;
import bench_framework;

#ifndef BIOSEQ_IO_REPORT_DIR
#error "BIOSEQ_IO_REPORT_DIR must be defined by the build system"
#endif
#ifndef BIOSEQ_IO_DATA_DIR
#error "BIOSEQ_IO_DATA_DIR must be defined by the build system"
#endif
#ifndef BIOSEQ_IO_BENCH_CONFIG_PATH
#error "BIOSEQ_IO_BENCH_CONFIG_PATH must be defined by the build system"
#endif

namespace fs = std::filesystem;

namespace {

using bioseq::bench_framework::ParamMap;
using bioseq::bench_framework::RunConstraints;
using bioseq::io::reader_options;
using bioseq::io::writer_options;
using bioseq::io::sequence_batch;
using bioseq::io::sequence_io_file_type;
using bioseq::io::sequence_writer;
using bioseq::io::read_sequences_file;

using BenchAlphabet = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
using BenchBatch    = sequence_batch<BenchAlphabet>;

/// Reader Input. Owns the on-disk corpus path. Synthetic corpora are
/// generated in `on_input` and deleted in `on_cleanup`. Real-data
/// corpora are referenced by absolute path; never deleted.
struct ReaderInput {
    std::string  corpus_path;
    std::size_t  bytes_on_disk        = 0;
    std::size_t  decompressed_bytes   = 0;
    std::size_t  expected_record_count = 0;
    bool         is_fastq             = false;
    bool         is_compressed        = false;
    bool         is_synthetic         = false;
};

/// Writer Input. Same shape as ReaderInput, but additionally the source
/// corpus is slurped into memory once so the timed loop only measures
/// the writer.
struct WriterInput {
    std::string  source_corpus_path;
    std::size_t  source_bytes_on_disk        = 0;
    std::size_t  source_record_count         = 0;
    bool         is_fastq                    = false;
    bool         is_synthetic_source         = false;

    /// Per-iteration scratch path. Each `on_run` call writes here, then
    /// `on_evaluate` re-reads to validate roundtrip integrity. Created
    /// once in `on_input`; deleted in `on_cleanup`.
    std::string  output_scratch_path;

    /// In-memory corpus for the writer loop. Populated in `on_input`.
    std::vector<BenchBatch> batches;

    /// XOR-aggregated content hash of `batches`, computed once in
    /// `on_input` for the FASTQ shape (= header || seq ASCII || qual
    /// truncated to encoded length). The `on_evaluate` lambda
    /// recomputes this from the writer's on-disk output and compares
    /// — any mismatch surfaces as `roundtrip_record_match = 0`.
    /// `_fastq` and `_fasta` variants because the FASTQ form folds in
    /// quality bytes; FASTA only header + seq.
    std::uint64_t expected_content_hash_fastq = 0;
    std::uint64_t expected_content_hash_fasta = 0;
};

struct IoOutput {
    std::size_t observed_record_count = 0;
    std::size_t observed_body_bytes   = 0;
    std::size_t bytes_written         = 0;     ///< writer outputs only
    std::size_t roundtrip_record_count = 0;    ///< writer outputs only
};

// DOC0295 §12.1 — identifier-parser overhead benchmark data model. Measures the
// parser path (resolve once + parse every header into a capture writer) against
// the legacy dumb whole-header copy, on the same synthetic FASTA corpus. Neither
// path touches Postgres, isolating IO/parser overhead from catalog/coercion.
struct IdentifierBenchInput {
    ReaderInput reader;
    bioseq::io::idparse::identifier_mapping_result mapping;
    bioseq::io::idparse::header_format effective_fmt =
        bioseq::io::idparse::header_format::UNIPROTKB;
};

struct IdentifierBenchOutput {
    std::size_t observed_record_count = 0;
    std::size_t observed_body_bytes   = 0;
    std::size_t header_bytes          = 0;
    std::size_t parsed_field_count    = 0;
    std::size_t parsed_field_bytes    = 0;
};

// DOC0377 §18 — one NO_POSTGRES benchmark-local expression backend. The
// production SQL piece tree owns PostgreSQL MemoryContexts and source domains,
// so bench-io exercises its representation-erased contract through the shared
// canonical facade and the production admitted packed materializer.
using LazyExpressionRange = compact_sequence_range<BenchAlphabet>;
using LazyExpressionFacade =
    bioseq::storage::basic_lazy_sequence_view<seq::dna4>;
using LazyExpressionPacked =
    bioseq::storage::admitted_packed_region<seq::dna4>;
using LazyExpressionSize = bioseq::storage::sequence_size_t;

inline constexpr std::uint64_t kLazyExpressionHashSeed =
    0x0377d1a5e5e0c0deULL;
inline constexpr std::uint8_t kLazyExpressionStorageWidth = 8u;

struct LazyExpressionInput {
    LazyExpressionRange source;
    LazyExpressionSize source_bp = 0;
    LazyExpressionSize expected_output_bp = 0;
    std::uint64_t expected_hash = 0;
};

struct LazyExpressionAllocationStats {
    std::uint64_t payload_materializations = 0;
    std::uint64_t intermediate_payload_materializations = 0;
    std::uint64_t boundary_materializations = 0;
    std::uint64_t payload_bytes_materialized = 0;
    std::uint64_t live_payload_bytes = 0;
    std::uint64_t peak_live_payload_bytes = 0;
    std::uint64_t admitted_bytes_total = 0;
    std::uint64_t live_admitted_bytes = 0;
    std::uint64_t peak_live_admitted_bytes = 0;
};

class LazyExpressionMaterialization {
public:
    LazyExpressionMaterialization(
            LazyExpressionPacked&& packed,
            LazyExpressionAllocationStats& stats,
            bool final_boundary,
            std::uint64_t payload_bytes,
            std::uint64_t observed_admitted_bytes) noexcept
        : packed_(std::move(packed)),
          stats_(std::addressof(stats)),
          payload_bytes_(payload_bytes),
          admitted_bytes_(observed_admitted_bytes) {
        ++stats.payload_materializations;
        if (final_boundary) {
            ++stats.boundary_materializations;
        } else {
            ++stats.intermediate_payload_materializations;
        }
        stats.payload_bytes_materialized += payload_bytes_;
        stats.live_payload_bytes += payload_bytes_;
        stats.peak_live_payload_bytes = std::max(
            stats.peak_live_payload_bytes, stats.live_payload_bytes);
        stats.admitted_bytes_total += admitted_bytes_;
        stats.live_admitted_bytes += admitted_bytes_;
        stats.peak_live_admitted_bytes = std::max(
            stats.peak_live_admitted_bytes, stats.live_admitted_bytes);
    }

    LazyExpressionMaterialization(
            LazyExpressionMaterialization&& other) noexcept
        : packed_(std::move(other.packed_)),
          stats_(std::exchange(other.stats_, nullptr)),
          payload_bytes_(other.payload_bytes_),
          admitted_bytes_(other.admitted_bytes_) {}

    LazyExpressionMaterialization(
        LazyExpressionMaterialization const&) = delete;
    LazyExpressionMaterialization& operator=(
        LazyExpressionMaterialization const&) = delete;
    LazyExpressionMaterialization& operator=(
        LazyExpressionMaterialization&&) = delete;

    ~LazyExpressionMaterialization() { release_tracking(); }

    [[nodiscard]] LazyExpressionFacade view() const noexcept {
        return packed_.view();
    }

private:
    void release_tracking() noexcept {
        if (stats_ == nullptr) {
            return;
        }
        DEBUG_ASSERT(
            stats_->live_payload_bytes >= payload_bytes_,
            "lazy-expression live payload accounting underflow");
        DEBUG_ASSERT(
            stats_->live_admitted_bytes >= admitted_bytes_,
            "lazy-expression live admission accounting underflow");
        stats_->live_payload_bytes -= payload_bytes_;
        stats_->live_admitted_bytes -= admitted_bytes_;
        stats_ = nullptr;
    }

    LazyExpressionPacked packed_;
    LazyExpressionAllocationStats* stats_ = nullptr;
    std::uint64_t payload_bytes_ = 0;
    std::uint64_t admitted_bytes_ = 0;
};

struct LazyExpressionOutput {
    LazyExpressionSize output_bp = 0;
    std::uint64_t logical_hash = 0;
    LazyExpressionAllocationStats allocations{};
};

enum class LazyExpressionNodeKind : std::uint8_t {
    slice,
    concat,
};

struct LazyExpressionNode {
    LazyExpressionNodeKind kind;
    LazyExpressionFacade left;
    LazyExpressionFacade right;
    LazyExpressionSize slice_from;
    LazyExpressionSize logical_size;
};

[[nodiscard]] LazyExpressionNode make_lazy_expression_slice(
        LazyExpressionFacade source,
        LazyExpressionSize from,
        LazyExpressionSize to) {
    if (from > to || to > source.bp_size()) {
        throw std::invalid_argument("invalid lazy-expression slice");
    }
    return LazyExpressionNode{
        .kind = LazyExpressionNodeKind::slice,
        .left = source,
        .right = source,
        .slice_from = from,
        .logical_size = to - from,
    };
}

[[nodiscard]] LazyExpressionNode make_lazy_expression_concat(
        LazyExpressionFacade left,
        LazyExpressionFacade right) {
    constexpr LazyExpressionSize max_size =
        (std::numeric_limits<LazyExpressionSize>::max)();
    if (right.bp_size() > max_size - left.bp_size()) {
        throw std::overflow_error("lazy-expression concat length overflow");
    }
    return LazyExpressionNode{
        .kind = LazyExpressionNodeKind::concat,
        .left = left,
        .right = right,
        .slice_from = 0,
        .logical_size = left.bp_size() + right.bp_size(),
    };
}

[[nodiscard]] LazyExpressionSize lazy_expression_node_bp_size(
        void const* opaque) noexcept {
    return static_cast<LazyExpressionNode const*>(opaque)->logical_size;
}

[[nodiscard]] bool lazy_expression_node_worker_shareable(
        void const* opaque) noexcept {
    const auto& node = *static_cast<LazyExpressionNode const*>(opaque);
    return lazy_is_worker_shareable(node.left)
        && lazy_is_worker_shareable(node.right);
}

[[nodiscard]] LazyExpressionFacade::resident_probe_result
lazy_expression_node_resident_probe(
        void const*, LazyExpressionSize, LazyExpressionSize,
        std::uint8_t) noexcept {
    return std::nullopt;
}

void lazy_expression_copy_child(
        LazyExpressionFacade child,
        LazyExpressionSize from,
        std::span<std::uint8_t> output) {
    constexpr auto max_size = (std::numeric_limits<std::size_t>::max)();
    if (from > max_size || output.size() > max_size - from) {
        throw std::overflow_error("lazy-expression coordinate overflow");
    }
    const std::size_t begin = static_cast<std::size_t>(from);
    child.template to_encoded_into<false, false>(
        output, begin, begin + output.size());
}

void lazy_expression_copy_forward(
        LazyExpressionNode const& node,
        LazyExpressionSize from,
        std::span<std::uint8_t> output) {
    const LazyExpressionSize count =
        static_cast<LazyExpressionSize>(output.size());
    if (from > node.logical_size || count > node.logical_size - from) {
        throw std::invalid_argument("invalid lazy-expression read range");
    }
    if (output.empty()) {
        return;
    }
    if (node.kind == LazyExpressionNodeKind::slice) {
        lazy_expression_copy_child(
            node.left, node.slice_from + from, output);
        return;
    }

    const LazyExpressionSize left_size = node.left.bp_size();
    if (from >= left_size) {
        lazy_expression_copy_child(node.right, from - left_size, output);
        return;
    }
    const std::size_t left_count = static_cast<std::size_t>(
        std::min<LazyExpressionSize>(count, left_size - from));
    lazy_expression_copy_child(node.left, from, output.first(left_count));
    if (left_count != output.size()) {
        lazy_expression_copy_child(
            node.right, 0, output.subspan(left_count));
    }
}

template <bool IsReversed, bool WriteReversed>
[[nodiscard]] const std::uint8_t* lazy_expression_node_ptr_at(
        void const* opaque,
        LazyExpressionSize from,
        std::size_t required_length,
        std::span<std::uint8_t> scratch) {
    const auto& node = *static_cast<LazyExpressionNode const*>(opaque);
    const LazyExpressionSize count =
        static_cast<LazyExpressionSize>(required_length);
    if (required_length > scratch.size()
        || from > node.logical_size
        || count > node.logical_size - from) {
        throw std::invalid_argument("invalid lazy-expression facade read");
    }
    LazyExpressionSize forward_from = from;
    if constexpr (IsReversed) {
        forward_from = node.logical_size - from - count;
    }
    std::span<std::uint8_t> output = scratch.first(required_length);
    lazy_expression_copy_forward(node, forward_from, output);
    if constexpr (IsReversed) {
        for (std::uint8_t& rank : output) {
            rank = LazyExpressionFacade::traits::letter_complement(rank);
        }
    }
    if constexpr (IsReversed != WriteReversed) {
        std::reverse(output.begin(), output.end());
    }
    return scratch.data();
}

const LazyExpressionFacade::backend_ops kLazyExpressionNodeOps{
    .bp_size = &lazy_expression_node_bp_size,
    .ptr_at = {
        &lazy_expression_node_ptr_at<false, false>,
        &lazy_expression_node_ptr_at<false, true>,
        &lazy_expression_node_ptr_at<true, false>,
        &lazy_expression_node_ptr_at<true, true>,
    },
    .try_resident_packed = &lazy_expression_node_resident_probe,
    .copy_full_packed = nullptr,
    .worker_shareable = &lazy_expression_node_worker_shareable,
};

[[nodiscard]] LazyExpressionFacade lazy_expression_view(
        LazyExpressionNode const& node) noexcept {
    return LazyExpressionFacade{
        std::addressof(node), std::addressof(kLazyExpressionNodeOps)};
}

[[nodiscard]] LazyExpressionMaterialization materialize_lazy_expression_view(
        LazyExpressionFacade view,
        bool final_boundary,
        LazyExpressionAllocationStats& stats) {
    auto& admission =
        bioseq::parallelism::MemoryAdmissionController::instance();
    const std::uint64_t admitted_bytes =
        bioseq::storage::packed_region_admission_bytes<seq::dna4>(
            view.bp_size(), kLazyExpressionStorageWidth);
    const std::uint64_t reserved_before = admission.reserved_bytes();
    auto permit = admission.try_reserve(admitted_bytes);
    if (!permit.has_value()) {
        throw std::runtime_error(
            "memory admission rejected lazy-expression materialization");
    }
    LazyExpressionPacked packed = bioseq::storage::materialize_packed_region(
        view, 0, view.bp_size(), kLazyExpressionStorageWidth,
        std::move(*permit));
    const std::uint64_t reserved_after = admission.reserved_bytes();
    if (reserved_after < reserved_before
        || reserved_after - reserved_before != admitted_bytes) {
        throw std::runtime_error(
            "lazy-expression observed admission delta mismatch");
    }
    const std::uint64_t observed_admitted_bytes =
        reserved_after - reserved_before;
    const auto& descriptor = packed.descriptor();
    if (descriptor.unit_count
            > (std::numeric_limits<std::uint64_t>::max)()
                / kLazyExpressionStorageWidth) {
        throw std::overflow_error("lazy-expression payload byte overflow");
    }
    const std::uint64_t payload_bytes =
        static_cast<std::uint64_t>(descriptor.unit_count)
        * kLazyExpressionStorageWidth;
    return LazyExpressionMaterialization{
        std::move(packed), stats, final_boundary, payload_bytes,
        observed_admitted_bytes};
}

[[nodiscard]] LazyExpressionOutput finish_lazy_expression_run(
        LazyExpressionMaterialization const& final_value) {
    const LazyExpressionFacade final_view = final_value.view();
    return LazyExpressionOutput{
        .output_bp = final_view.bp_size(),
        .logical_hash = bioseq::storage::sequence_logical_hash_v1(
            final_view, kLazyExpressionHashSeed),
    };
}

void validate_lazy_expression_run_released(
        LazyExpressionAllocationStats const& stats,
        std::uint64_t reserved_baseline) {
    const std::uint64_t reserved_after =
        bioseq::parallelism::MemoryAdmissionController::instance()
            .reserved_bytes();
    if (stats.live_payload_bytes != 0u
        || stats.live_admitted_bytes != 0u
        || reserved_after != reserved_baseline) {
        throw std::runtime_error(
            "lazy-expression materialization ownership did not return to baseline");
    }
}

struct bench_identifier_writer {
    std::size_t dest_n = 0;
    IdentifierBenchOutput* out = nullptr;
    std::vector<std::string> scratch;   // simulates per-slot materialisation

    explicit bench_identifier_writer(std::size_t n, IdentifierBenchOutput& o)
        : dest_n(n), out(&o), scratch(n) {}

    void begin_tuple() { for (auto& s : scratch) s.clear(); }
    void set_field(bioseq::io::idparse::dest_slot_t slot,
                   const char* data, std::int32_t len) {
        auto& s = scratch.at(static_cast<std::size_t>(slot));
        s.assign(data, static_cast<std::size_t>(len));   // same shape as dumb copy
        ++out->parsed_field_count;
        out->parsed_field_bytes += s.size();
    }
    void set_null(bioseq::io::idparse::dest_slot_t slot) {
        scratch.at(static_cast<std::size_t>(slot)).clear();
    }
    void end_tuple() {}
    void finish() {}
    [[nodiscard]] std::size_t dest_count() const noexcept { return dest_n; }
};
static_assert(bioseq::io::idparse::is_identifier_dest_writer<bench_identifier_writer>);

[[nodiscard]] std::int64_t read_int_field(simdjson::dom::element data,
                                            std::string_view name,
                                            std::int64_t fallback) noexcept {
    std::int64_t value = fallback;
    std::int64_t parsed = 0;
    if (data[name].get(parsed) == simdjson::SUCCESS) {
        value = parsed;
    }
    return value;
}

[[nodiscard]] std::string read_string_field(simdjson::dom::element data,
                                              std::string_view name,
                                              std::string_view fallback) {
    std::string_view raw{};
    if (data[name].get(raw) == simdjson::SUCCESS) {
        return std::string{raw};
    }
    return std::string{fallback};
}

[[nodiscard]] bool read_bool_field(simdjson::dom::element data,
                                    std::string_view name,
                                    bool fallback) noexcept {
    bool value = fallback;
    bool parsed = false;
    if (data[name].get(parsed) == simdjson::SUCCESS) {
        value = parsed;
    }
    return value;
}

[[nodiscard]] std::vector<seq::dna4> make_lazy_expression_source(
        std::size_t bp_size,
        std::uint64_t seed) {
    std::vector<seq::dna4> source(bp_size);
    std::uint64_t state = seed;
    for (seq::dna4& letter : source) {
        state = state * 6364136223846793005ULL
            + 1442695040888963407ULL;
        letter.assign_rank(static_cast<seqan3::alphabet_rank_t<seq::dna4>>(
            (state >> 33u) & 0x3u));
    }
    return source;
}

void append_lazy_expression_oracle_segment(
        std::vector<std::uint8_t>& oracle,
        std::span<const seq::dna4> source,
        std::size_t from,
        std::size_t to) {
    if (from > to || to > source.size()) {
        throw std::invalid_argument("invalid lazy-expression oracle segment");
    }
    for (const seq::dna4 letter : source.subspan(from, to - from)) {
        oracle.push_back(static_cast<std::uint8_t>(letter.to_rank()));
    }
}

[[nodiscard]] LazyExpressionInput build_lazy_expression_input(
        std::string_view,
        std::string_view,
        simdjson::dom::element data,
        const RunConstraints&) {
    const std::int64_t configured_bp = read_int_field(
        data, "bp_size", 8 * 1024 * 1024);
    if (configured_bp <= 0
        || configured_bp % 8 != 0
        || static_cast<std::uint64_t>(configured_bp)
            > (std::numeric_limits<std::size_t>::max)()) {
        throw std::invalid_argument(
            "lazy-expression bp_size must be a positive multiple of eight");
    }
    const std::size_t n = static_cast<std::size_t>(configured_bp);
    const std::uint64_t seed = static_cast<std::uint64_t>(
        read_int_field(data, "seed", 377));
    std::vector<seq::dna4> source = make_lazy_expression_source(n, seed);

    // Independent flattening of
    //   concat(concat(source[3N/8:5N/8], source[N/2:3N/4]),
    //          source[N/4:N/2]).
    // This is algebraically equivalent to the benchmark expression, without
    // using its slice/concat backend or materializer.
    std::vector<std::uint8_t> oracle;
    oracle.reserve(3u * n / 4u);
    const std::span<const seq::dna4> source_span{source};
    append_lazy_expression_oracle_segment(
        oracle, source_span, 3u * n / 8u, 5u * n / 8u);
    append_lazy_expression_oracle_segment(
        oracle, source_span, n / 2u, 3u * n / 4u);
    append_lazy_expression_oracle_segment(
        oracle, source_span, n / 4u, n / 2u);
    const std::uint64_t expected_hash =
        bioseq::storage::sequence_logical_hash_bytes_v1(
            oracle, static_cast<std::uint64_t>(oracle.size()),
            kLazyExpressionHashSeed);

    return LazyExpressionInput{
        .source = LazyExpressionRange{source},
        .source_bp = static_cast<LazyExpressionSize>(n),
        .expected_output_bp = static_cast<LazyExpressionSize>(oracle.size()),
        .expected_hash = expected_hash,
    };
}

[[nodiscard]] RunConstraints estimate_lazy_expression_resources(
        std::string_view,
        std::string_view,
        simdjson::dom::element data) noexcept {
    const std::int64_t configured_bp = read_int_field(
        data, "bp_size", 8 * 1024 * 1024);
    if (configured_bp <= 0) {
        return {};
    }
    const std::uint64_t bp = static_cast<std::uint64_t>(configured_bp);
    constexpr std::uint64_t max_size =
        (std::numeric_limits<std::size_t>::max)();
    const std::uint64_t reserved = bp > max_size / 2u
        ? max_size : bp * 2u;
    return RunConstraints{
        .base_memory = static_cast<std::size_t>(reserved),
        .extra_memory = static_cast<std::size_t>(reserved),
    };
}

[[nodiscard]] LazyExpressionOutput run_lazy_expression_eager(
        const LazyExpressionInput& input) {
    const LazyExpressionSize n = input.source_bp;
    auto source_owner = bioseq::storage::make_lazy_in_memory(input.source);
    const LazyExpressionFacade source =
        bioseq::storage::as_lazy_sequence_view(source_owner);
    LazyExpressionAllocationStats stats{};
    const std::uint64_t reserved_baseline =
        bioseq::parallelism::MemoryAdmissionController::instance()
            .reserved_bytes();
    LazyExpressionOutput output{};
    {
        const LazyExpressionNode a_node = make_lazy_expression_slice(
            source, n / 8u, 5u * n / 8u);
        const LazyExpressionFacade a_expression = lazy_expression_view(a_node);
        const LazyExpressionMaterialization a = materialize_lazy_expression_view(
            a_expression, false, stats);

        const LazyExpressionNode b_node = make_lazy_expression_slice(
            source, n / 2u, 7u * n / 8u);
        const LazyExpressionFacade b_expression = lazy_expression_view(b_node);
        const LazyExpressionMaterialization b = materialize_lazy_expression_view(
            b_expression, false, stats);

        const LazyExpressionNode c_node = make_lazy_expression_concat(
            a.view(), b.view());
        const LazyExpressionFacade c_expression = lazy_expression_view(c_node);
        const LazyExpressionMaterialization c = materialize_lazy_expression_view(
            c_expression, false, stats);

        const LazyExpressionNode d_node = make_lazy_expression_slice(
            c.view(), n / 4u, 3u * n / 4u);
        const LazyExpressionFacade d_expression = lazy_expression_view(d_node);
        const LazyExpressionMaterialization d = materialize_lazy_expression_view(
            d_expression, false, stats);

        const LazyExpressionNode e_node = make_lazy_expression_slice(
            a.view(), n / 8u, 3u * n / 8u);
        const LazyExpressionFacade e_expression = lazy_expression_view(e_node);
        const LazyExpressionMaterialization e = materialize_lazy_expression_view(
            e_expression, false, stats);

        const LazyExpressionNode result_node = make_lazy_expression_concat(
            d.view(), e.view());
        const LazyExpressionFacade result_expression =
            lazy_expression_view(result_node);
        const LazyExpressionMaterialization result =
            materialize_lazy_expression_view(result_expression, true, stats);
        output = finish_lazy_expression_run(result);
    }
    validate_lazy_expression_run_released(stats, reserved_baseline);
    output.allocations = stats;
    return output;
}

[[nodiscard]] LazyExpressionOutput run_lazy_expression_deferred(
        const LazyExpressionInput& input) {
    const LazyExpressionSize n = input.source_bp;
    auto source_owner = bioseq::storage::make_lazy_in_memory(input.source);
    const LazyExpressionFacade source =
        bioseq::storage::as_lazy_sequence_view(source_owner);
    LazyExpressionAllocationStats stats{};
    const std::uint64_t reserved_baseline =
        bioseq::parallelism::MemoryAdmissionController::instance()
            .reserved_bytes();
    LazyExpressionOutput output{};
    {
        const LazyExpressionNode a_node = make_lazy_expression_slice(
            source, n / 8u, 5u * n / 8u);
        const LazyExpressionFacade a = lazy_expression_view(a_node);
        const LazyExpressionNode b_node = make_lazy_expression_slice(
            source, n / 2u, 7u * n / 8u);
        const LazyExpressionFacade b = lazy_expression_view(b_node);
        const LazyExpressionNode c_node = make_lazy_expression_concat(a, b);
        const LazyExpressionFacade c = lazy_expression_view(c_node);
        const LazyExpressionNode d_node = make_lazy_expression_slice(
            c, n / 4u, 3u * n / 4u);
        const LazyExpressionFacade d = lazy_expression_view(d_node);
        const LazyExpressionNode e_node = make_lazy_expression_slice(
            a, n / 8u, 3u * n / 8u);
        const LazyExpressionFacade e = lazy_expression_view(e_node);
        const LazyExpressionNode result_node =
            make_lazy_expression_concat(d, e);
        const LazyExpressionFacade result_expression =
            lazy_expression_view(result_node);
        const LazyExpressionMaterialization result =
            materialize_lazy_expression_view(result_expression, true, stats);
        output = finish_lazy_expression_run(result);
    }
    validate_lazy_expression_run_released(stats, reserved_baseline);
    output.allocations = stats;
    return output;
}

[[nodiscard]] LazyExpressionOutput run_lazy_expression(
        std::string_view algorithm,
        const ParamMap&,
        const LazyExpressionInput& input) {
    if (algorithm == "bioseq_lazy_expr_eager_each_step") {
        return run_lazy_expression_eager(input);
    }
    if (algorithm == "bioseq_lazy_expr_deferred") {
        return run_lazy_expression_deferred(input);
    }
    throw std::invalid_argument("unknown lazy-expression benchmark algorithm");
}

[[nodiscard]] constexpr std::uint64_t lazy_expression_payload_bytes(
        LazyExpressionSize bp) noexcept {
    constexpr std::uint64_t letters_per_unit = 32u;
    const std::uint64_t units = bp / letters_per_unit
        + static_cast<std::uint64_t>(bp % letters_per_unit != 0u);
    return units * kLazyExpressionStorageWidth;
}

[[nodiscard]] std::uint64_t expected_lazy_expression_payload_bytes(
        bool eager,
        LazyExpressionSize n) noexcept {
    if (!eager) {
        return lazy_expression_payload_bytes(3u * n / 4u);
    }
    return lazy_expression_payload_bytes(n / 2u)
        + lazy_expression_payload_bytes(3u * n / 8u)
        + lazy_expression_payload_bytes(7u * n / 8u)
        + lazy_expression_payload_bytes(n / 2u)
        + lazy_expression_payload_bytes(n / 4u)
        + lazy_expression_payload_bytes(3u * n / 4u);
}

[[nodiscard]] std::uint64_t expected_lazy_expression_admitted_bytes(
        bool eager,
        LazyExpressionSize n) {
    const auto admitted = [](LazyExpressionSize bp) {
        return bioseq::storage::packed_region_admission_bytes<seq::dna4>(
            bp, kLazyExpressionStorageWidth);
    };
    if (!eager) {
        return admitted(3u * n / 4u);
    }
    return admitted(n / 2u)
        + admitted(3u * n / 8u)
        + admitted(7u * n / 8u)
        + admitted(n / 2u)
        + admitted(n / 4u)
        + admitted(3u * n / 4u);
}

[[nodiscard]] std::map<std::string, double> evaluate_lazy_expression(
        std::string_view algorithm,
        const ParamMap&,
        const LazyExpressionInput& input,
        const LazyExpressionOutput& output) {
    const bool eager = algorithm == "bioseq_lazy_expr_eager_each_step";
    const std::uint64_t expected_materializations = eager ? 6u : 1u;
    const std::uint64_t expected_intermediate = eager ? 5u : 0u;
    const std::uint64_t expected_payload =
        expected_lazy_expression_payload_bytes(eager, input.source_bp);
    const std::uint64_t expected_admitted =
        expected_lazy_expression_admitted_bytes(eager, input.source_bp);
    const auto& allocations = output.allocations;
    const bool correctness_ok =
        output.output_bp == input.expected_output_bp
        && output.logical_hash == input.expected_hash;
    const bool allocation_ok =
        allocations.payload_materializations == expected_materializations
        && allocations.intermediate_payload_materializations
            == expected_intermediate
        && allocations.boundary_materializations == 1u
        && allocations.payload_bytes_materialized == expected_payload
        && allocations.live_payload_bytes == 0u
        && allocations.peak_live_payload_bytes == expected_payload
        && allocations.admitted_bytes_total == expected_admitted
        && allocations.live_admitted_bytes == 0u
        && allocations.peak_live_admitted_bytes == expected_admitted;
    if (!correctness_ok || !allocation_ok) {
        throw std::runtime_error(
            "lazy-expression correctness/allocation contract failed");
    }

    std::map<std::string, double> metrics;
    metrics.emplace("output_bp", static_cast<double>(output.output_bp));
    metrics.emplace("logical_hash_hi32", static_cast<double>(
        static_cast<std::uint32_t>(output.logical_hash >> 32u)));
    metrics.emplace("logical_hash_lo32", static_cast<double>(
        static_cast<std::uint32_t>(output.logical_hash)));
    metrics.emplace("payload_materializations", static_cast<double>(
        allocations.payload_materializations));
    metrics.emplace("intermediate_payload_materializations", static_cast<double>(
        allocations.intermediate_payload_materializations));
    metrics.emplace("boundary_materializations", static_cast<double>(
        allocations.boundary_materializations));
    metrics.emplace("payload_bytes_materialized", static_cast<double>(
        allocations.payload_bytes_materialized));
    metrics.emplace("peak_live_payload_bytes", static_cast<double>(
        allocations.peak_live_payload_bytes));
    metrics.emplace("admitted_bytes_total", static_cast<double>(
        allocations.admitted_bytes_total));
    metrics.emplace("peak_live_admitted_bytes", static_cast<double>(
        allocations.peak_live_admitted_bytes));
    metrics.emplace("correctness_contract_ok", correctness_ok ? 1.0 : 0.0);
    metrics.emplace("allocation_contract_ok", allocation_ok ? 1.0 : 0.0);
    return metrics;
}

void cleanup_lazy_expression(
        std::string_view,
        std::string_view,
        simdjson::dom::element,
        const LazyExpressionInput&) noexcept {}

/// Allocate a unique scratch path under the system temp directory.
[[nodiscard]] std::string allocate_temp_path(std::string_view tag) {
    static std::atomic<std::uint64_t> counter{0};
    fs::path temp_directory_root = fs::temp_directory_path();
    fs::path full_path = temp_directory_root / fmt::format(
        "bioseq_iobench_{}_{}_{}_{}",
        tag, ::getpid(),
        counter.fetch_add(1, std::memory_order_relaxed),
        std::random_device{}() & 0xFFFFFFu);
    return full_path.string();
}

/// Write a synthetic FASTA / FASTQ corpus to `output_path`. Returns
/// the number of bytes actually written.
[[nodiscard]] std::size_t emit_synthetic_corpus(const std::string& output_path,
                                                  bool is_fastq,
                                                  std::size_t record_count,
                                                  std::size_t mean_record_length_bp,
                                                  std::uint64_t seed,
                                                  std::string_view header_shape = "plain") {
    std::ofstream out(output_path, std::ios::binary);
    if (!out) {
        bioseq_log<ERR>("[io_bench_v10] failed to open '{}' for writing", output_path);
        return 0;
    }
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int> base_dist(0, 3);
    std::normal_distribution<double> length_dist(
        static_cast<double>(mean_record_length_bp),
        static_cast<double>(mean_record_length_bp) * 0.1);
    constexpr std::array<char, 4> bases{'A', 'C', 'G', 'T'};
    constexpr std::size_t fasta_wrap = 80;

    std::string buffer;
    buffer.reserve(64 * 1024);
    std::size_t bytes_written = 0;
    auto flush_buffer = [&]() {
        if (buffer.empty()) return;
        out.write(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        bytes_written += buffer.size();
        buffer.clear();
    };

    for (std::size_t record_index = 0; record_index < record_count; ++record_index) {
        const double sample = length_dist(rng);
        const std::size_t record_length = static_cast<std::size_t>(
            std::max(8.0, sample));
        // DOC0295 §12.1 — `header_shape` defaults to "plain" so every existing IO
        // benchmark cell is byte-for-byte unchanged unless it opts in. The
        // "uniprot_metadata" shape exercises positional fields, a multi-word
        // description, EQUALS tags, alias routing and metadata stripping at once.
        if (header_shape == "uniprot_metadata" && !is_fastq) {
            buffer.append(fmt::format(
                ">tr|P{:06}|P{:06}_BENCH Synthetic parser benchmark protein "
                "OS=Homo sapiens OX=9606 PE=1 SV={}\n",
                record_index, record_index, 1 + (record_index % 9)));
        } else if (is_fastq) {
            buffer.append(fmt::format("@seq_{}\n", record_index));
        } else {
            buffer.append(fmt::format(">seq_{}\n", record_index));
        }
        if (buffer.size() >= 32 * 1024) flush_buffer();

        if (is_fastq) {
            for (std::size_t letter_index = 0; letter_index < record_length; ++letter_index) {
                buffer.push_back(bases[base_dist(rng)]);
            }
            buffer.push_back('\n');
            buffer.push_back('+');
            buffer.push_back('\n');
            buffer.append(record_length, 'I');
            buffer.push_back('\n');
            if (buffer.size() >= 32 * 1024) flush_buffer();
        } else {
            std::size_t emitted_letters = 0;
            while (emitted_letters < record_length) {
                const std::size_t this_line =
                    std::min(fasta_wrap, record_length - emitted_letters);
                for (std::size_t letter_index = 0; letter_index < this_line; ++letter_index) {
                    buffer.push_back(bases[base_dist(rng)]);
                }
                buffer.push_back('\n');
                emitted_letters += this_line;
                if (buffer.size() >= 32 * 1024) flush_buffer();
            }
        }
    }
    flush_buffer();
    out.flush();
    out.close();
    return bytes_written;
}

/// Common attributes resolved from a test-case `data` blob: where the
/// corpus lives, how big it is, whether it was generated locally (and
/// thus needs `on_cleanup` to delete it). Returned by
/// `prepare_corpus`; reader / writer `on_input` lambdas copy whichever
/// fields they care about into their own `Input` struct.
struct CorpusDescriptor {
    std::string path;
    std::size_t bytes_on_disk      = 0;
    std::size_t record_count       = 0;  ///< 0 for real-data (unknown until parse)
    bool        is_fastq           = false;
    bool        is_compressed      = false;
    bool        is_synthetic       = false;
    bool        valid              = false;
};

/// Resolve a corpus's on-disk path. For real-data cells, look up the
/// path under `data_dir`; for synthetic, generate to a temp path.
[[nodiscard]] CorpusDescriptor prepare_corpus(std::string_view data_dir,
                                                std::string_view test_case_name,
                                                simdjson::dom::element data) {
    CorpusDescriptor descriptor;
    const std::string format_name = read_string_field(data, "format", "fasta");
    descriptor.is_fastq = (format_name == "fastq");
    descriptor.is_compressed = read_bool_field(data, "compressed", false);
    const std::string kind = read_string_field(data, "kind", "synthetic");

    if (kind == "real") {
        const std::string relative = read_string_field(data, "real_data_path", "");
        if (relative.empty()) {
            bioseq_log<ERR>("[io_bench_v10] test case '{}' missing real_data_path",
                  test_case_name);
            return descriptor;
        }
        fs::path joined = fs::path{std::string{data_dir}} / relative;
        std::error_code ec;
        if (!fs::exists(joined, ec)) {
            bioseq_log<ERR>("[io_bench_v10] real-data file missing for '{}': {}",
                  test_case_name, joined.string());
            return descriptor;
        }
        const auto observed_size = fs::file_size(joined, ec);
        if (ec) {
            bioseq_log<ERR>("[io_bench_v10] cannot stat '{}': {}",
                  joined.string(), ec.message());
            return descriptor;
        }
        descriptor.path = joined.string();
        descriptor.bytes_on_disk = static_cast<std::size_t>(observed_size);
        descriptor.is_synthetic = false;
        descriptor.valid = true;
        return descriptor;
    }
    if (kind != "synthetic") {
        bioseq_log<ERR>("[io_bench_v10] test case '{}' has unknown kind '{}'",
              test_case_name, kind);
        return descriptor;
    }

    const std::size_t record_count = static_cast<std::size_t>(
        read_int_field(data, "record_count", 1000));
    const std::size_t mean_record_length_bp = static_cast<std::size_t>(
        read_int_field(data, "mean_record_length_bp", 200));
    const std::uint64_t seed = static_cast<std::uint64_t>(
        read_int_field(data, "seed", 42));
    const std::string header_shape =
        read_string_field(data, "header_shape", "plain");

    const std::string path_tag =
        std::string{format_name} + "_" + std::string{test_case_name};
    descriptor.path = allocate_temp_path(path_tag);
    const std::size_t bytes = emit_synthetic_corpus(
        descriptor.path, descriptor.is_fastq,
        record_count, mean_record_length_bp, seed, header_shape);
    if (bytes == 0) {
        bioseq_log<ERR>("[io_bench_v10] synthetic corpus generation failed for '{}'",
              test_case_name);
        descriptor.path.clear();
        return descriptor;
    }
    descriptor.bytes_on_disk = bytes;
    descriptor.record_count  = record_count;
    descriptor.is_synthetic  = true;
    descriptor.valid         = true;
    return descriptor;
}

/// Best-effort estimate of total bytes that one batch of this test case
/// will need to keep in memory.  For synthetic corpora we multiply
/// record_count by mean_record_length_bp; for real-data corpora we
/// stat the on-disk file (and scale up by the canonical ~3.2x to
/// approximate the decompressed size when `compressed: true`).
///
/// Used by both `estimate_reader_resources` and
/// `estimate_writer_resources` so the two paths share the same sizing
/// model.  Returns zero if neither shape is determinable, in which case
/// the framework treats the batch as zero-cost — strictly worse than a
/// non-zero estimate, but acceptable: the run continues, it just doesn't
/// participate in budget throttling.
[[nodiscard]] std::size_t estimate_corpus_bytes(
    std::string_view data_dir,
    simdjson::dom::element data) noexcept
{
    const std::string kind = read_string_field(data, "kind", "synthetic");
    if (kind == "real") {
        const std::string relative = read_string_field(data, "real_data_path", "");
        if (relative.empty()) return 0;
        fs::path joined = fs::path{std::string{data_dir}} / relative;
        std::error_code ec;
        const auto observed_size = fs::file_size(joined, ec);
        if (ec) return 0;
        std::size_t bytes = static_cast<std::size_t>(observed_size);
        if (read_bool_field(data, "compressed", false)) {
            // Same factor `build_reader_input` uses for `decompressed_bytes`.
            bytes = static_cast<std::size_t>(static_cast<double>(bytes) * 3.2);
        }
        return bytes;
    }
    if (kind != "synthetic") return 0;
    const auto record_count = static_cast<std::size_t>(
        read_int_field(data, "record_count", 1000));
    const auto mean_record_length_bp = static_cast<std::size_t>(
        read_int_field(data, "mean_record_length_bp", 200));
    return record_count * mean_record_length_bp;
}

/// Reader-side resource estimator (see `main/DOCS/BENCHMARK_FRAMEWORK/BENCH_V11_IMPL_PLAN__DOC0224.md` §3.5 /
/// §4 and `main/DOCS/BENCHMARK_FRAMEWORK/BENCH_V12_IMPL_PLAN__DOC0226.md` §3.4).  The reader's `Input` only
/// holds the corpus path plus a few size counters, so the persistent
/// (`base_memory`) cost is small.  However, we still budget `2x` the
/// total corpus bytes for both base + extra so the dynamic memory
/// budget throttles concurrent batches: the actual reader paths
/// (mmap'd regions, parallel-gzip ring buffers, decoded sequence
/// batches handed to the per-batch handler) push real RAM pressure
/// during `on_run`, even though the framework can't see those
/// allocations from the outside.
[[nodiscard]] RunConstraints estimate_reader_resources(
    std::string_view data_dir,
    std::string_view /*test_case_name*/,
    simdjson::dom::element data) noexcept
{
    const std::size_t bytes = estimate_corpus_bytes(data_dir, data);
    return {
        .base_memory  = bytes * 2,
        .extra_memory = bytes * 2,
    };
}

/// Writer-side resource estimator.  Differs from the reader because
/// `WriterInput` slurps every input batch into RAM (see
/// `WriterInput::batches`), so the persistent footprint *is* really
/// `~bytes` bytes.  Same `2x base + 2x extra` shape as the reader for
/// uniformity and headroom; callers can rely on it as a known
/// "this batch needs ~4x corpus bytes free" gate.  Compressed cells
/// are reader-only (`build_writer_input` early-returns) but we still
/// emit a sensible estimate so the framework doesn't think those
/// batches are free.
[[nodiscard]] RunConstraints estimate_writer_resources(
    std::string_view data_dir,
    std::string_view /*test_case_name*/,
    simdjson::dom::element data) noexcept
{
    const std::size_t bytes = estimate_corpus_bytes(data_dir, data);
    return {
        .base_memory  = bytes * 2,
        .extra_memory = bytes * 2,
    };
}

// =====================================================================
//  Reader runners
// =====================================================================
//
// One `dispatch_reader_*` function per reader algorithm. Each returns
// the IoOutput populated with observed_record_count + observed_body_bytes.

template <bool IsFasta>
[[nodiscard]] IoOutput run_bioseq_io_with_options(const std::string& path,
                                                    const reader_options& options) {
    using alphabet_config = BenchAlphabet;
    using batch_t = sequence_batch<alphabet_config>;
    constexpr auto file_type = IsFasta
        ? sequence_io_file_type::FASTA
        : sequence_io_file_type::FASTQ;

    std::atomic<std::size_t> record_count_atomic{0};
    std::atomic<std::size_t> body_bytes_atomic{0};

    auto handler_fn = [&record_count_atomic, &body_bytes_atomic](batch_t&& batch) {
        const std::size_t local_records = batch.size();
        std::size_t local_body_bytes = 0;
        for (std::size_t record_index = 0; record_index < local_records; ++record_index) {
            const auto seq_view = batch.sequence(record_index);
            using traits = typename decltype(seq_view)::traits;
            local_body_bytes += seq_view.size() * traits::letters_per_unit
                                 - seq_view.padding();
        }
        record_count_atomic.fetch_add(local_records, std::memory_order_relaxed);
        body_bytes_atomic.fetch_add(local_body_bytes, std::memory_order_relaxed);
    };
    auto failure_fn = [&path](std::string&& msg) {
        bioseq_log<ERR>("[io_bench_v10] read failed on '{}': {}", path, msg);
    };
    auto outcome = read_sequences_file<seq::dna4, file_type>(
        path, handler_fn, failure_fn, options);
    if (!outcome.has_value()) {
        bioseq_log<ERR>("[io_bench_v10] read_sequences_file error: {}", outcome.error());
    }
    IoOutput out;
    out.observed_record_count = record_count_atomic.load(std::memory_order_relaxed);
    out.observed_body_bytes   = body_bytes_atomic.load(std::memory_order_relaxed);
    return out;
}

[[nodiscard]] IoOutput run_kseqpp(const std::string& path) {
    IoOutput out;
    klibpp::SeqStreamIn stream(path.c_str());
    klibpp::KSeq record;
    while (stream >> record) {
        out.observed_body_bytes += record.seq.size();
        out.observed_record_count += 1;
    }
    return out;
}

[[nodiscard]] IoOutput run_naive_ifstream_fasta(const std::string& path) {
    IoOutput out;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        bioseq_log<ERR>("[io_bench_v10] naive ifstream open failed: '{}'", path);
        return out;
    }
    std::string line;
    bool in_record = false;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        if (line.front() == '>') {
            if (in_record) out.observed_record_count += 1;
            in_record = true;
            continue;
        }
        if (in_record) {
            std::size_t body_size = line.size();
            if (body_size > 0 && line[body_size - 1] == '\r') body_size -= 1;
            out.observed_body_bytes += body_size;
        }
    }
    if (in_record) out.observed_record_count += 1;
    return out;
}

[[nodiscard]] IoOutput run_naive_ifstream_fastq(const std::string& path) {
    IoOutput out;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        bioseq_log<ERR>("[io_bench_v10] naive ifstream open failed: '{}'", path);
        return out;
    }
    std::string line;
    enum class FastqStep : std::uint8_t { Header, Seq, Sep, Qual };
    FastqStep step = FastqStep::Header;
    while (std::getline(in, line)) {
        switch (step) {
            case FastqStep::Header:
                if (line.empty() || line.front() != '@') continue;
                step = FastqStep::Seq;
                break;
            case FastqStep::Seq: {
                std::size_t body_size = line.size();
                if (body_size > 0 && line[body_size - 1] == '\r') body_size -= 1;
                out.observed_body_bytes += body_size;
                step = FastqStep::Sep;
                break;
            }
            case FastqStep::Sep:
                step = FastqStep::Qual;
                break;
            case FastqStep::Qual:
                out.observed_record_count += 1;
                step = FastqStep::Header;
                break;
        }
    }
    return out;
}

[[nodiscard]] IoOutput dispatch_reader(std::string_view algo,
                                         const ReaderInput& input) {
    reader_options options{};
    if (algo == "bioseq_io_sync_fasta") {
        options.io.gzip_auto_detect = false;
        options.io.prefer_mmap      = false;
        options.concurrency.worker_count = 0;
        return run_bioseq_io_with_options<true>(input.corpus_path, options);
    }
    if (algo == "bioseq_io_mmap_fasta") {
        options.io.gzip_auto_detect = false;
        options.io.prefer_mmap      = true;
        options.concurrency.worker_count = 0;
        return run_bioseq_io_with_options<true>(input.corpus_path, options);
    }
    if (algo == "bioseq_io_async4_fasta") {
        options.io.gzip_auto_detect = false;
        options.io.prefer_mmap      = false;
        options.concurrency.worker_count = 4;
        return run_bioseq_io_with_options<true>(input.corpus_path, options);
    }
    if (algo == "bioseq_io_sync_fastq") {
        options.io.gzip_auto_detect = false;
        options.io.prefer_mmap      = false;
        options.concurrency.worker_count = 0;
        return run_bioseq_io_with_options<false>(input.corpus_path, options);
    }
    if (algo == "bioseq_io_mmap_fastq") {
        options.io.gzip_auto_detect = false;
        options.io.prefer_mmap      = true;
        options.concurrency.worker_count = 0;
        return run_bioseq_io_with_options<false>(input.corpus_path, options);
    }
    if (algo == "bioseq_io_async4_fastq") {
        options.io.gzip_auto_detect = false;
        options.io.prefer_mmap      = false;
        options.concurrency.worker_count = 4;
        return run_bioseq_io_with_options<false>(input.corpus_path, options);
    }
    if (algo == "bioseq_io_pgzip_fastq") {
        options.io.gzip_auto_detect = true;
        options.io.prefer_mmap      = true;
        options.io.parallel_gzip_enabled = true;
        options.io.parallel_gzip_thread_count = 0;
        options.concurrency.worker_count = 0;
        return run_bioseq_io_with_options<false>(input.corpus_path, options);
    }
    if (algo == "bioseq_io_zlib_fastq") {
        options.io.gzip_auto_detect = true;
        options.io.prefer_mmap      = true;
        options.io.parallel_gzip_enabled = false;
        options.concurrency.worker_count = 0;
        return run_bioseq_io_with_options<false>(input.corpus_path, options);
    }
    if (algo == "kseqpp_fasta" || algo == "kseqpp_fastq") {
        return run_kseqpp(input.corpus_path);
    }
    if (algo == "naive_ifstream_fasta") {
        return run_naive_ifstream_fasta(input.corpus_path);
    }
    if (algo == "naive_ifstream_fastq") {
        return run_naive_ifstream_fastq(input.corpus_path);
    }
    bioseq_log<ERR>("[io_bench_v10] unknown reader algorithm '{}'", algo);
    return {};
}

// =====================================================================
//  Reader benchmark lambdas
// =====================================================================

[[nodiscard]] ReaderInput build_reader_input(std::string_view data_dir,
                                                std::string_view test_case_name,
                                                simdjson::dom::element data,
                                                const RunConstraints& /*constraints*/) {
    const CorpusDescriptor descriptor =
        prepare_corpus(data_dir, test_case_name, data);
    ReaderInput input;
    if (!descriptor.valid) return input;
    input.corpus_path           = descriptor.path;
    input.bytes_on_disk         = descriptor.bytes_on_disk;
    input.expected_record_count = descriptor.record_count;
    input.is_fastq              = descriptor.is_fastq;
    input.is_compressed         = descriptor.is_compressed;
    input.is_synthetic          = descriptor.is_synthetic;
    // For gzipped real-data: estimate decompressed size at ~3.2x compressed
    // (matches the SRR FASTQ corpus shape — empirical 6.0 GiB / 1.96 GiB).
    input.decompressed_bytes = input.is_compressed
        ? static_cast<std::size_t>(input.bytes_on_disk * 3.2)
        : input.bytes_on_disk;
    bioseq_log<DBG>("[io_bench_v10] reader corpus ready: '{}' (path={}, bytes={})",
            test_case_name, input.corpus_path, input.bytes_on_disk);
    return input;
}

[[nodiscard]] IoOutput run_reader(std::string_view algo,
                                    const ParamMap& /*params*/,
                                    const ReaderInput& input) {
    if (input.corpus_path.empty()) return {};
    return dispatch_reader(algo, input);
}

[[nodiscard]] std::map<std::string, double> evaluate_reader(
    std::string_view /*algo*/,
    const ParamMap& /*params*/,
    const ReaderInput& input,
    const IoOutput& output)
{
    std::map<std::string, double> metrics;
    metrics.emplace("bytes_on_disk", static_cast<double>(input.bytes_on_disk));
    metrics.emplace("decompressed_bytes", static_cast<double>(input.decompressed_bytes));
    metrics.emplace("record_count", static_cast<double>(output.observed_record_count));
    // Body bytes observed by the reader. For dna4-on-dna5 corpora this
    // is < raw seq bytes (N's get dropped); for dna4-on-dna4 it equals
    // the raw seq bytes. Surface as a metric so equality rules between
    // bioseq paths can detect a path silently dropping or duplicating
    // bytes that the others see.
    metrics.emplace("observed_body_bytes",
                    static_cast<double>(output.observed_body_bytes));
    // Sanity: the reader must observe at least one record on a
    // non-empty corpus. Surface as a 0/1 metric so the regression
    // layer can pin it to 1 across iterations and test cases.
    metrics.emplace("records_nonzero",
                    output.observed_record_count > 0 ? 1.0 : 0.0);
    return metrics;
}

void cleanup_reader(std::string_view /*data_dir*/,
                    std::string_view /*test_case_name*/,
                    simdjson::dom::element /*data*/,
                    const ReaderInput& input)
{
    if (!input.is_synthetic || input.corpus_path.empty()) return;
    std::error_code ec;
    fs::remove(input.corpus_path, ec);
}

// =====================================================================
//  Identifier-parser overhead benchmark (DOC0295 §12.1)
// =====================================================================

[[nodiscard]] IdentifierBenchInput build_identifier_input(
        std::string_view data_dir, std::string_view test_case_name,
        simdjson::dom::element data, const RunConstraints& constraints) {
    IdentifierBenchInput input;
    input.reader = build_reader_input(data_dir, test_case_name, data, constraints);
    if (input.reader.corpus_path.empty()) return input;

    using namespace bioseq::io::idparse;
    const std::vector<column_descriptor> columns{
        {.name="header",              .is_text=true,  .is_primary_key=false, .not_null=false, .has_default=false, .is_identity_or_generated=false},
        {.name="accession",           .is_text=true,  .is_primary_key=false, .not_null=false, .has_default=false, .is_identity_or_generated=false},
        {.name="organism_name",       .is_text=true,  .is_primary_key=false, .not_null=false, .has_default=false, .is_identity_or_generated=false},
        {.name="organism_identifier", .is_text=false, .is_primary_key=false, .not_null=false, .has_default=false, .is_identity_or_generated=false},
        {.name="protein_existence",   .is_text=false, .is_primary_key=false, .not_null=false, .has_default=false, .is_identity_or_generated=false},
        {.name="sequence_version",    .is_text=false, .is_primary_key=false, .not_null=false, .has_default=false, .is_identity_or_generated=false},
    };
    auto map = resolve_identifier_mapping(
        header_format::UNIPROTKB, header_format::UNIPROTKB, columns,
        std::span<const user_column_mapping>{});
    if (!map) {
        bioseq_log<ERR>("[io_bench_v10] identifier mapping failed: {}", map.error());
        input.reader.corpus_path.clear();
        return input;
    }
    input.mapping = std::move(*map);
    return input;
}

[[nodiscard]] IdentifierBenchOutput run_identifier_bench(
        std::string_view algo, const ParamMap& /*params*/,
        const IdentifierBenchInput& input) {
    IdentifierBenchOutput out;
    if (input.reader.corpus_path.empty()) return out;

    using namespace bioseq::io;
    using namespace bioseq::io::idparse;
    using alphabet_config = BenchAlphabet;
    using batch_t = sequence_batch<alphabet_config>;

    std::string dumb_header_copy;
    identifier_parse_state parse_state;
    bench_identifier_writer parsed_writer{input.mapping.routing.dest_count, out};
    const auto writer_ref = make_identifier_writer_ref(parsed_writer);

    auto handler_fn = [&](batch_t&& batch) {
        for (std::size_t i = 0; i < batch.size(); ++i) {
            const auto header = batch.header(i);
            const auto seq_view = batch.sequence(i);
            using traits = typename decltype(seq_view)::traits;
            out.observed_body_bytes +=
                seq_view.size() * traits::letters_per_unit - seq_view.padding();
            ++out.observed_record_count;
            out.header_bytes += header.size();

            if (algo == "bioseq_identifier_parse_uniprot_fasta") {
                parsed_writer.begin_tuple();
                auto parsed = parse_and_route_header(
                    header_format::UNIPROTKB, header,
                    input.mapping.routing, parse_state, writer_ref);
                if (!parsed)
                    bioseq_log<ERR>("[io_bench_v10] identifier parse failed: {}", parsed.error());
                parsed_writer.end_tuple();
            } else {  // bioseq_dumb_header_fasta
                dumb_header_copy.assign(header.data(), header.size());
            }
        }
    };
    auto failure_fn = [](std::string&& msg) {
        bioseq_log<ERR>("[io_bench_v10] identifier benchmark read failed: {}", msg);
    };
    reader_options opts{};
    opts.io.gzip_auto_detect = false;
    opts.io.prefer_mmap = false;
    opts.concurrency.worker_count = 0;
    (void) read_sequences_file<seq::dna4, sequence_io_file_type::FASTA>(
        input.reader.corpus_path, handler_fn, failure_fn, opts);
    return out;
}

[[nodiscard]] std::map<std::string, double> evaluate_identifier_bench(
        std::string_view /*algo*/, const ParamMap& /*params*/,
        const IdentifierBenchInput& input, const IdentifierBenchOutput& out) {
    return {
        {"bytes_on_disk", static_cast<double>(input.reader.bytes_on_disk)},
        {"record_count", static_cast<double>(out.observed_record_count)},
        {"observed_body_bytes", static_cast<double>(out.observed_body_bytes)},
        {"records_nonzero", out.observed_record_count > 0 ? 1.0 : 0.0},
        {"header_bytes", static_cast<double>(out.header_bytes)},
        {"parsed_field_count", static_cast<double>(out.parsed_field_count)},
        {"parsed_field_bytes", static_cast<double>(out.parsed_field_bytes)},
    };
}

void cleanup_identifier_bench(std::string_view data_dir,
                              std::string_view test_case_name,
                              simdjson::dom::element data,
                              const IdentifierBenchInput& input) {
    cleanup_reader(data_dir, test_case_name, data, input.reader);
}

// =====================================================================
//  Writer benchmark lambdas
// =====================================================================

/// XOR-hash a single record's content (header || '\1' || decoded
/// sequence ASCII || '\1' || quality bytes truncated to encoded length).
/// Returns a 64-bit hash. XOR is order-invariant — the parallel writer
/// reorders records across producer threads, so a hash based on a sorted
/// list would be hostile to that path; XOR of per-record hashes does not
/// care about order.
///
/// The truncation `qsize = min(quality.size(), total_letters)` mirrors
/// `sequence_writer::write_batch`: when the input alphabet drops bytes
/// (e.g. dna4 reading dna5 inputs silently drops 'N's), the encoded
/// sequence is shorter than the raw quality span, and the writer
/// truncates qual to total_letters. We hash the same truncated form
/// here so input and output hashes agree under that contract.
///
/// Hash collisions on a single record are statistically unlikely
/// (1/2^64); after XORing N records, the chance of a false-positive
/// match remains far below 1 in 10^9 for realistic N.
template <bool IsFastq>
[[nodiscard]] std::uint64_t hash_record_xor(
    std::string_view header,
    const auto& sequence_view,
    std::span<const std::uint8_t> quality)
{
    using seq_traits = typename std::remove_cvref_t<decltype(sequence_view)>::traits;
    constexpr std::size_t letters_per_word = seq_traits::letters_per_unit;
    const std::size_t total_letters =
        sequence_view.size() * letters_per_word - sequence_view.padding();
    std::string serialized;
    serialized.reserve(header.size() + total_letters
                       + (IsFastq ? quality.size() : 0) + 2);
    serialized.append(header);
    serialized.push_back('\1');
    for (std::size_t letter_index = 0; letter_index < total_letters; ++letter_index) {
        const auto word = sequence_view.data()[letter_index / letters_per_word];
        const std::size_t letter_in_word = letter_index % letters_per_word;
        const std::uint8_t rank = static_cast<std::uint8_t>(
            (word >> (seq_traits::bits_per_char * letter_in_word))
            & seq_traits::char_mask);
        serialized.push_back(seq_traits::letter_to_char(rank));
    }
    if constexpr (IsFastq) {
        serialized.push_back('\1');
        const std::size_t qsize = std::min(quality.size(), total_letters);
        serialized.append(reinterpret_cast<const char*>(quality.data()), qsize);
    }
    return std::hash<std::string>{}(serialized);
}

/// Compute the XOR-aggregated content hash of every record across all
/// batches. Order-invariant — safe to use as the canonical content
/// fingerprint for the writer's input AND for the re-read output (the
/// parallel writer may reorder records but cannot change the SET of
/// records). Returns 0 for an empty batch list.
template <bool IsFastq>
[[nodiscard]] std::uint64_t hash_batches_xor(
    const std::vector<BenchBatch>& batches)
{
    std::uint64_t accumulator = 0;
    for (const auto& batch : batches) {
        const std::size_t record_count_in_batch = batch.size();
        for (std::size_t record_index = 0;
             record_index < record_count_in_batch; ++record_index) {
            const auto seq_view = batch.sequence(record_index);
            const auto header_view = batch.header(record_index);
            std::span<const std::uint8_t> qual_view{};
            if constexpr (IsFastq) {
                qual_view = batch.quality(record_index);
            }
            accumulator ^= hash_record_xor<IsFastq>(
                header_view, seq_view, qual_view);
        }
    }
    return accumulator;
}

/// Slurp `path` into an in-memory list of batches via the bioseq reader.
template <bool IsFasta>
[[nodiscard]] bool slurp_corpus(const std::string& path,
                                  std::vector<BenchBatch>& out_batches,
                                  std::size_t& out_record_count) {
    constexpr auto file_type = IsFasta
        ? sequence_io_file_type::FASTA
        : sequence_io_file_type::FASTQ;

    out_batches.clear();
    out_record_count = 0;

    auto handler_fn = [&](BenchBatch&& batch) {
        out_record_count += batch.size();
        out_batches.push_back(std::move(batch));
    };
    auto failure_fn = [&path](std::string&& msg) {
        bioseq_log<ERR>("[io_bench_v10] slurp failed on '{}': {}", path, msg);
    };
    reader_options options{};
    options.io.gzip_auto_detect = false;
    options.io.prefer_mmap      = true;
    options.concurrency.worker_count = 0;

    auto outcome = read_sequences_file<seq::dna4, file_type>(
        path, handler_fn, failure_fn, options);
    if (!outcome.has_value()) {
        bioseq_log<ERR>("[io_bench_v10] slurp_corpus read error: {}", outcome.error());
        return false;
    }
    return !out_batches.empty();
}

[[nodiscard]] WriterInput build_writer_input(std::string_view data_dir,
                                                std::string_view test_case_name,
                                                simdjson::dom::element data,
                                                const RunConstraints& /*constraints*/) {
    WriterInput input;

    // The .fastq.gz cell is reader-only; writers don't have a compress
    // path. Skip silently.
    const bool compressed_hint = read_bool_field(data, "compressed", false);
    if (compressed_hint) {
        bioseq_log<DBG>("[io_bench_v10] writer skipping gz cell '{}'", test_case_name);
        return input;
    }
    const CorpusDescriptor descriptor =
        prepare_corpus(data_dir, test_case_name, data);
    if (!descriptor.valid) return input;
    input.source_corpus_path    = descriptor.path;
    input.source_bytes_on_disk  = descriptor.bytes_on_disk;
    input.source_record_count   = descriptor.record_count;
    input.is_fastq              = descriptor.is_fastq;
    input.is_synthetic_source   = descriptor.is_synthetic;

    bool slurp_ok = false;
    if (input.is_fastq) {
        slurp_ok = slurp_corpus<false>(
            input.source_corpus_path, input.batches, input.source_record_count);
    } else {
        slurp_ok = slurp_corpus<true>(
            input.source_corpus_path, input.batches, input.source_record_count);
    }
    if (!slurp_ok) {
        bioseq_log<ERR>("[io_bench_v10] writer slurp failed for '{}' — clearing input",
              test_case_name);
        input.batches.clear();
        return input;
    }
    // Compute the expected content hash once. `on_evaluate` will
    // re-read the writer's output and compute the same hash; mismatches
    // signal silent corruption (truncated qualities, header-without-body
    // chunks, dropped records, etc.).
    if (input.is_fastq) {
        input.expected_content_hash_fastq = hash_batches_xor<true>(input.batches);
    } else {
        input.expected_content_hash_fasta = hash_batches_xor<false>(input.batches);
    }
    input.output_scratch_path = allocate_temp_path(
        std::string{"writer_out_"} + std::string{test_case_name});
    bioseq_log<DBG>("[io_bench_v10] writer corpus ready: '{}' (records={}, scratch={})",
            test_case_name, input.source_record_count, input.output_scratch_path);
    return input;
}

template <bool IsFasta>
[[nodiscard]] IoOutput run_bioseq_writer(const WriterInput& input,
                                           std::size_t thread_count,
                                           bool use_mmap_sink = false) {
    constexpr auto file_type = IsFasta
        ? sequence_io_file_type::FASTA
        : sequence_io_file_type::FASTQ;
    writer_options options{};
    if (use_mmap_sink) {
        // Pick a generous upper bound for the output size — has to be
        // large enough that no record write overflows the mmap'd
        // region, but sparse-file metadata makes overestimation cheap.
        // 1.5× the slurped raw bytes leaves headroom for FASTA wrap
        // newlines / FASTQ separator bytes the writer adds back, even
        // after accounting for multi-line bodies in the source corpus
        // collapsing to single-line bodies in the writer's output.
        const std::size_t estimated_output_bytes =
            input.source_bytes_on_disk + input.source_bytes_on_disk / 2;
        options.output_capacity_hint =
            estimated_output_bytes > 4096 ? estimated_output_bytes : 4096;
    }
    auto writer_or_err = sequence_writer<seq::dna4, file_type>::open(
        input.output_scratch_path, options);
    if (!writer_or_err) {
        bioseq_log<ERR>("[io_bench_v10] writer open failed: {}", writer_or_err.error());
        return {};
    }
    auto& writer = *writer_or_err;
    const std::size_t batches_total = input.batches.size();
    if (thread_count <= 1 || batches_total <= 1) {
        for (const auto& batch : input.batches) {
            if (!writer.write_batch(batch)) {
                bioseq_log<ERR>("[io_bench_v10] writer write_batch failed");
                (void)writer.close();
                return {};
            }
        }
    } else {
        const std::size_t resolved_threads =
            thread_count > batches_total ? batches_total : thread_count;
        std::atomic<bool> any_failure{false};
        auto run_shard = [&](std::size_t shard_index) {
            const std::size_t shard_start =
                (batches_total * shard_index) / resolved_threads;
            const std::size_t shard_end =
                (batches_total * (shard_index + 1)) / resolved_threads;
            for (std::size_t batch_index = shard_start; batch_index < shard_end; ++batch_index) {
                if (!writer.write_batch(input.batches[batch_index])) {
                    any_failure.store(true, std::memory_order_relaxed);
                    return;
                }
            }
        };
        std::vector<std::thread> shard_threads;
        shard_threads.reserve(resolved_threads);
        for (std::size_t shard_index = 0; shard_index < resolved_threads; ++shard_index) {
            shard_threads.emplace_back(run_shard, shard_index);
        }
        for (auto& thread : shard_threads) thread.join();
        if (any_failure.load(std::memory_order_relaxed)) {
            (void)writer.close();
            return {};
        }
    }
    auto close_outcome = writer.close();
    if (!close_outcome) {
        bioseq_log<ERR>("[io_bench_v10] writer close failed: {}", close_outcome.error());
        return {};
    }
    IoOutput out;
    out.observed_record_count = input.source_record_count;
    std::error_code ec;
    out.bytes_written = fs::file_size(input.output_scratch_path, ec);
    return out;
}

template <bool IsFasta>
[[nodiscard]] IoOutput run_naive_ofstream(const WriterInput& input) {
    std::ofstream out(input.output_scratch_path, std::ios::binary);
    if (!out) {
        bioseq_log<ERR>("[io_bench_v10] naive ofstream open failed: '{}'",
              input.output_scratch_path);
        return {};
    }
    std::string scratch;
    scratch.reserve(64 * 1024);
    constexpr std::size_t fasta_wrap = 80;

    for (const auto& batch : input.batches) {
        const std::size_t record_count_in_batch = batch.size();
        for (std::size_t record_index = 0; record_index < record_count_in_batch; ++record_index) {
            scratch.clear();
            const auto seq_view = batch.sequence(record_index);
            const auto header_view = batch.header(record_index);
            using seq_traits = typename std::remove_cvref_t<decltype(seq_view)>::traits;
            constexpr std::size_t letters_per_word = seq_traits::letters_per_unit;
            const std::size_t total_letters =
                seq_view.size() * letters_per_word - seq_view.padding();
            scratch.push_back(IsFasta ? '>' : '@');
            scratch.append(header_view);
            scratch.push_back('\n');
            std::size_t in_line_count = 0;
            for (std::size_t letter_index = 0; letter_index < total_letters; ++letter_index) {
                const auto word = seq_view.data()[letter_index / letters_per_word];
                const std::size_t letter_in_word = letter_index % letters_per_word;
                const std::uint8_t rank = static_cast<std::uint8_t>(
                    (word >> (seq_traits::bits_per_char * letter_in_word))
                    & seq_traits::char_mask);
                // Use the alphabet's canonical letter_to_char mapping
                // so we don't bake in the dna4 LUT shape — works for
                // any alphabet supported by `BenchAlphabet`.
                scratch.push_back(seq_traits::letter_to_char(rank));
                if constexpr (IsFasta) {
                    ++in_line_count;
                    if (in_line_count == fasta_wrap &&
                        letter_index + 1 < total_letters) {
                        scratch.push_back('\n');
                        in_line_count = 0;
                    }
                }
            }
            if constexpr (!IsFasta) {
                scratch.push_back('\n');
                scratch.push_back('+');
                scratch.push_back('\n');
                const auto quality_view = batch.quality(record_index);
                // Truncate quality to match the encoded sequence length.
                // Mirrors `sequence_writer::write_batch`: when the input
                // alphabet drops bytes (e.g. dna4 reading dna5 inputs
                // silently drops 'N's), the encoded sequence is shorter
                // than the raw quality span. Writing the full
                // quality_view here would emit qual_len > seq_len,
                // causing the FASTQ FSM to abort on re-read with
                // "FASTQ qual length mismatch (qual=X, seq=Y)".
                const std::size_t qsize = std::min(quality_view.size(),
                                                    total_letters);
                scratch.append(reinterpret_cast<const char*>(quality_view.data()),
                                qsize);
                if (qsize < total_letters) {
                    scratch.append(total_letters - qsize, '!');
                }
            }
            scratch.push_back('\n');
            out.write(scratch.data(), static_cast<std::streamsize>(scratch.size()));
        }
    }
    out.flush();
    out.close();
    IoOutput result;
    result.observed_record_count = input.source_record_count;
    std::error_code ec;
    result.bytes_written = fs::file_size(input.output_scratch_path, ec);
    return result;
}

[[nodiscard]] IoOutput dispatch_writer(std::string_view algo,
                                         const WriterInput& input) {
    if (algo == "bioseq_writer_sync_fasta")      return run_bioseq_writer<true>(input, 1);
    if (algo == "bioseq_writer_parallel4_fasta") return run_bioseq_writer<true>(input, 4);
    if (algo == "bioseq_writer_parallel8_fasta") return run_bioseq_writer<true>(input, 8);
    // mmap_pwrite_sink path: writer.cppm `output_capacity_hint > 0` —
    // bypasses the kernel pwrite memcpy by writing into a mmap'd region
    // that the OS flushes asynchronously. Validates the second sink
    // implementation in sinks.cppm. Uses 4 producer threads to also
    // exercise the per-thread chunk path against the mmap sink.
    if (algo == "bioseq_writer_mmap_fasta")      return run_bioseq_writer<true>(input, 4, /*use_mmap_sink=*/true);
    if (algo == "bioseq_writer_sync_fastq")      return run_bioseq_writer<false>(input, 1);
    if (algo == "bioseq_writer_parallel4_fastq") return run_bioseq_writer<false>(input, 4);
    if (algo == "bioseq_writer_parallel8_fastq") return run_bioseq_writer<false>(input, 8);
    if (algo == "bioseq_writer_mmap_fastq")      return run_bioseq_writer<false>(input, 4, /*use_mmap_sink=*/true);
    if (algo == "naive_ofstream_fasta")          return run_naive_ofstream<true>(input);
    if (algo == "naive_ofstream_fastq")          return run_naive_ofstream<false>(input);
    bioseq_log<ERR>("[io_bench_v10] unknown writer algorithm '{}'", algo);
    return {};
}

[[nodiscard]] IoOutput run_writer(std::string_view algo,
                                    const ParamMap& /*params*/,
                                    const WriterInput& input) {
    if (input.batches.empty() || input.output_scratch_path.empty()) return {};
    return dispatch_writer(algo, input);
}

/// Re-read the writer's output to count records AND verify byte-faithful
/// content. Returns 1.0 only if both:
///   1. record count matches the input slurp;
///   2. the XOR-aggregated content hash matches the input slurp's hash.
/// Either failure returns 0.0 — the bench then surfaces it as a
/// roundtrip violation. This catches silent corruption that the v10
/// "count-only" check missed (e.g. truncated quality in the naive
/// writer, headers split from bodies by mid-record flushes).
template <bool IsFasta>
[[nodiscard]] double roundtrip_match(const std::string& output_path,
                                       std::size_t expected_record_count,
                                       std::uint64_t expected_content_hash) {
    if (output_path.empty()) return 0.0;
    std::error_code ec;
    if (!fs::exists(output_path, ec) || fs::file_size(output_path, ec) == 0) {
        return 0.0;
    }
    constexpr auto file_type = IsFasta
        ? sequence_io_file_type::FASTA
        : sequence_io_file_type::FASTQ;
    std::atomic<std::size_t> record_count{0};
    std::atomic<std::uint64_t> content_hash{0};
    auto handler_fn = [&record_count, &content_hash](BenchBatch&& batch) {
        record_count.fetch_add(batch.size(), std::memory_order_relaxed);
        // Hash this batch's contribution and XOR into the global
        // accumulator. XOR is associative + commutative, so concurrent
        // batch handlers contribute correctly via fetch_xor.
        const std::uint64_t batch_hash = hash_batches_xor<!IsFasta>(
            std::vector<BenchBatch>{std::move(batch)});
        content_hash.fetch_xor(batch_hash, std::memory_order_relaxed);
    };
    auto failure_fn = [&output_path](std::string&& msg) {
        bioseq_log<ERR>("[io_bench_v10] roundtrip read failed on '{}': {}",
              output_path, msg);
    };
    reader_options options{};
    options.io.gzip_auto_detect = false;
    options.io.prefer_mmap      = true;
    options.concurrency.worker_count = 0;
    auto outcome = read_sequences_file<seq::dna4, file_type>(
        output_path, handler_fn, failure_fn, options);
    if (!outcome.has_value()) return 0.0;
    const std::size_t observed_records =
        record_count.load(std::memory_order_relaxed);
    const std::uint64_t observed_hash =
        content_hash.load(std::memory_order_relaxed);
    if (observed_records != expected_record_count) {
        bioseq_log<ERR>("[io_bench_v10] roundtrip record count mismatch on '{}': "
             "expected={}, observed={}",
             output_path, expected_record_count, observed_records);
        return 0.0;
    }
    if (observed_hash != expected_content_hash) {
        bioseq_log<ERR>("[io_bench_v10] roundtrip content hash mismatch on '{}': "
             "expected=0x{:016x}, observed=0x{:016x}",
             output_path, expected_content_hash, observed_hash);
        return 0.0;
    }
    return 1.0;
}

[[nodiscard]] std::map<std::string, double> evaluate_writer(
    std::string_view /*algo*/,
    const ParamMap& /*params*/,
    const WriterInput& input,
    const IoOutput& output)
{
    std::map<std::string, double> metrics;
    metrics.emplace("bytes_on_disk", static_cast<double>(output.bytes_written));
    metrics.emplace("record_count", static_cast<double>(output.observed_record_count));
    // bytes_written must be > 0 — a zero-byte output is a writer failure
    // even when no FSM error fires on re-read (empty file = 0 records,
    // record-count "matches" if expected_record_count is also 0). We
    // surface this as a separate metric so the regression layer can
    // pin it to a non-zero floor.
    metrics.emplace("output_bytes_nonzero",
                    output.bytes_written > 0 ? 1.0 : 0.0);
    const double match = input.is_fastq
        ? roundtrip_match<false>(input.output_scratch_path,
                                  input.source_record_count,
                                  input.expected_content_hash_fastq)
        : roundtrip_match<true>(input.output_scratch_path,
                                 input.source_record_count,
                                 input.expected_content_hash_fasta);
    metrics.emplace("roundtrip_record_match", match);
    return metrics;
}

void cleanup_writer(std::string_view /*data_dir*/,
                    std::string_view /*test_case_name*/,
                    simdjson::dom::element /*data*/,
                    const WriterInput& input)
{
    std::error_code ec;
    if (input.is_synthetic_source && !input.source_corpus_path.empty()) {
        fs::remove(input.source_corpus_path, ec);
    }
    if (!input.output_scratch_path.empty()) {
        fs::remove(input.output_scratch_path, ec);
    }
}

}  // namespace

int main(int argc, char** argv) {
    log_configure(true);
    fmtlog::setLogLevel(fmtlog::INF);

    const std::string config_path{BIOSEQ_IO_BENCH_CONFIG_PATH};
    const std::string report_dir{BIOSEQ_IO_REPORT_DIR};
    const std::string data_dir{BIOSEQ_IO_DATA_DIR};

    using bioseq::bench_framework::create_benchmark;

    using Estimate = std::function<RunConstraints(std::string_view,
                                                    std::string_view,
                                                    simdjson::dom::element)>;
    using ReaderBuild = std::function<ReaderInput(std::string_view,
                                                    std::string_view,
                                                    simdjson::dom::element,
                                                    const RunConstraints&)>;
    using ReaderRun = std::function<IoOutput(std::string_view,
                                                const ParamMap&,
                                                const ReaderInput&)>;
    using ReaderEval = std::function<std::map<std::string, double>(
        std::string_view, const ParamMap&,
        const ReaderInput&, const IoOutput&)>;
    using ReaderCleanup = std::function<void(std::string_view,
                                                std::string_view,
                                                simdjson::dom::element,
                                                const ReaderInput&)>;

    using WriterBuild = std::function<WriterInput(std::string_view,
                                                    std::string_view,
                                                    simdjson::dom::element,
                                                    const RunConstraints&)>;
    using WriterRun = std::function<IoOutput(std::string_view,
                                                const ParamMap&,
                                                const WriterInput&)>;
    using WriterEval = std::function<std::map<std::string, double>(
        std::string_view, const ParamMap&,
        const WriterInput&, const IoOutput&)>;
    using WriterCleanup = std::function<void(std::string_view,
                                                std::string_view,
                                                simdjson::dom::element,
                                                const WriterInput&)>;

    // DOC0295 §12.1 — identifier-parser overhead benchmark function types.
    using IdentifierBuild = std::function<IdentifierBenchInput(
        std::string_view, std::string_view, simdjson::dom::element,
        const RunConstraints&)>;
    using IdentifierRun = std::function<IdentifierBenchOutput(
        std::string_view, const ParamMap&, const IdentifierBenchInput&)>;
    using IdentifierEval = std::function<std::map<std::string, double>(
        std::string_view, const ParamMap&,
        const IdentifierBenchInput&, const IdentifierBenchOutput&)>;
    using IdentifierCleanup = std::function<void(
        std::string_view, std::string_view, simdjson::dom::element,
        const IdentifierBenchInput&)>;

    using LazyExpressionBuild = std::function<LazyExpressionInput(
        std::string_view, std::string_view, simdjson::dom::element,
        const RunConstraints&)>;
    using LazyExpressionRun = std::function<LazyExpressionOutput(
        std::string_view, const ParamMap&, const LazyExpressionInput&)>;
    using LazyExpressionEval = std::function<std::map<std::string, double>(
        std::string_view, const ParamMap&,
        const LazyExpressionInput&, const LazyExpressionOutput&)>;
    using LazyExpressionCleanup = std::function<void(
        std::string_view, std::string_view, simdjson::dom::element,
        const LazyExpressionInput&)>;

    return bioseq::bench_framework::run(
        argc, argv, report_dir, data_dir,
        create_benchmark<ReaderInput, IoOutput>(
            "IO readers — FASTA (plain / async / mmap)",
            "bench_io_readers_fasta", config_path,
            Estimate{estimate_reader_resources},
            ReaderBuild{build_reader_input},
            ReaderRun{run_reader},
            ReaderEval{evaluate_reader},
            ReaderCleanup{cleanup_reader}),
        create_benchmark<ReaderInput, IoOutput>(
            "IO readers — FASTQ (plain / async / mmap)",
            "bench_io_readers_fastq", config_path,
            Estimate{estimate_reader_resources},
            ReaderBuild{build_reader_input},
            ReaderRun{run_reader},
            ReaderEval{evaluate_reader},
            ReaderCleanup{cleanup_reader}),
        create_benchmark<ReaderInput, IoOutput>(
            "IO readers — gzipped paths",
            "bench_io_readers_gz", config_path,
            Estimate{estimate_reader_resources},
            ReaderBuild{build_reader_input},
            ReaderRun{run_reader},
            ReaderEval{evaluate_reader},
            ReaderCleanup{cleanup_reader}),
        create_benchmark<WriterInput, IoOutput>(
            "IO writers — FASTA (sync / parallel)",
            "bench_io_writers_fasta", config_path,
            Estimate{estimate_writer_resources},
            WriterBuild{build_writer_input},
            WriterRun{run_writer},
            WriterEval{evaluate_writer},
            WriterCleanup{cleanup_writer}),
        create_benchmark<WriterInput, IoOutput>(
            "IO writers — FASTQ (sync / parallel)",
            "bench_io_writers_fastq", config_path,
            Estimate{estimate_writer_resources},
            WriterBuild{build_writer_input},
            WriterRun{run_writer},
            WriterEval{evaluate_writer},
            WriterCleanup{cleanup_writer}),
        create_benchmark<IdentifierBenchInput, IdentifierBenchOutput>(
            "IO identifier parser — UniProt header overhead",
            "bench_io_identifier_parser", config_path,
            Estimate{estimate_reader_resources},
            IdentifierBuild{build_identifier_input},
            IdentifierRun{run_identifier_bench},
            IdentifierEval{evaluate_identifier_bench},
            IdentifierCleanup{cleanup_identifier_bench}),
        create_benchmark<LazyExpressionInput, LazyExpressionOutput>(
            "Lazy sequence expression — eager versus deferred materialization",
            "bench_io_lazy_sequence_expression", config_path,
            Estimate{estimate_lazy_expression_resources},
            LazyExpressionBuild{build_lazy_expression_input},
            LazyExpressionRun{run_lazy_expression},
            LazyExpressionEval{evaluate_lazy_expression},
            LazyExpressionCleanup{cleanup_lazy_expression}));
}
