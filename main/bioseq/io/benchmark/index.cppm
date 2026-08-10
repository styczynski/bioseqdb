/**
 * @file index.cppm
 * @brief Sequence IO benchmark definitions.
 *
 * Benchmark blocks declared in the benchmark configuration:
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
module;

#include <kseq++/seqio.hpp>
#include <libassert/assert.hpp>
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

export module bioseq.io.benchmark;

import bioseq.common.logging;
import bioseq.common.seqan_adapter;
import bioseq.sequences;
import bioseq.storage.lazy_sequence;
import bioseq.storage.size;
import bioseq.parallelism.memory_admission;
import bioseq.io.reader;
import bioseq.io.options;
import bioseq.io.writer;
import bioseq.io.identifier_parser.formats;
import bioseq.io.identifier_parser;
import bioseq.bench.framework;

#ifndef BIOSEQ_IO_REPORT_DIR
#error "BIOSEQ_IO_REPORT_DIR must be defined by the build system"
#endif
#ifndef BIOSEQ_IO_DATA_DIR
#error "BIOSEQ_IO_DATA_DIR must be defined by the build system"
#endif
#ifndef BIOSEQ_IO_BENCH_CONFIG_PATH
#error "BIOSEQ_IO_BENCH_CONFIG_PATH must be defined by the build system"
#endif

namespace filesystem = std::filesystem;

namespace {

using parameter_map = bioseq::bench::framework::parameter_map;
using run_constraints = bioseq::bench::framework::run_constraints;
using bioseq::io::reader_options;
using bioseq::io::writer_options;
using bioseq::io::sequence_batch;
using bioseq::io::sequence_file_format;
using bioseq::io::sequence_writer;
using bioseq::io::read_sequences_file;

using benchmark_alphabet = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
using benchmark_batch    = sequence_batch<benchmark_alphabet>;

/// Reader Input. Owns the on-disk corpus path. Synthetic corpora are
/// generated in `on_input` and deleted in `on_cleanup`. Real-data
/// corpora are referenced by absolute path; never deleted.
struct reader_input {
    std::string  corpus_path;
    std::size_t  bytes_on_disk        = 0;
    std::size_t  decompressed_bytes   = 0;
    std::size_t  expected_record_count = 0;
    bool         is_fastq             = false;
    bool         is_compressed        = false;
    bool         is_synthetic         = false;
};

/// Writer Input. Same shape as reader_input, but additionally the source
/// corpus is slurped into memory once so the timed loop only measures
/// the writer.
struct writer_input {
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
    std::vector<benchmark_batch> batches;

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

struct io_measurement {
    std::size_t observed_record_count = 0;
    std::size_t observed_body_bytes   = 0;
    std::size_t bytes_written         = 0;     ///< writer outputs only
    std::size_t roundtrip_record_count = 0;    ///< writer outputs only
};

// Identifier-parser overhead benchmark data model. Measures resolution plus
// per-header extraction against whole-header copying on the same synthetic
// FASTA corpus, without PostgreSQL catalog or coercion work.
struct identifier_benchmark_input {
    reader_input reader;
    bioseq::io::identifier_parser::identifier_mapping_result mapping;
    bioseq::io::identifier_parser::header_format effective_format =
        bioseq::io::identifier_parser::header_format::uniprotkb;
};

struct identifier_benchmark_measurement {
    std::size_t observed_record_count = 0;
    std::size_t observed_body_bytes   = 0;
    std::size_t header_bytes          = 0;
    std::size_t parsed_field_count    = 0;
    std::size_t parsed_field_bytes    = 0;
};

// PostgreSQL-independent benchmark-local expression backend. The
// production SQL piece tree owns PostgreSQL MemoryContexts and source domains,
// so bench-io exercises its representation-erased contract through the shared
// canonical facade and the production admitted packed materializer.
using lazy_expression_range = compact_sequence_range<benchmark_alphabet>;
using lazy_expression_facade =
    bioseq::storage::lazy_sequence_view<seq::dna4>;
using lazy_expression_packed =
    bioseq::storage::admitted_packed_sequence<seq::dna4>;
using lazy_expression_size = bioseq::storage::sequence_size_t;

inline constexpr std::uint64_t lazy_expression_hash_seed =
    0x0377d1a5e5e0c0deULL;
inline constexpr std::uint8_t lazy_expression_storage_width = 8u;

struct lazy_expression_input {
    lazy_expression_range source;
    lazy_expression_size source_logical_length = 0;
    lazy_expression_size expected_output_logical_length = 0;
    std::uint64_t expected_hash = 0;
};

struct lazy_expression_allocation_statistics {
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

class lazy_expression_materialization {
public:
    lazy_expression_materialization(
            lazy_expression_packed&& packed,
            lazy_expression_allocation_statistics& stats,
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

    lazy_expression_materialization(
            lazy_expression_materialization&& other) noexcept
        : packed_(std::move(other.packed_)),
          stats_(std::exchange(other.stats_, nullptr)),
          payload_bytes_(other.payload_bytes_),
          admitted_bytes_(other.admitted_bytes_) {}

    lazy_expression_materialization(
        lazy_expression_materialization const&) = delete;
    lazy_expression_materialization& operator=(
        lazy_expression_materialization const&) = delete;
    lazy_expression_materialization& operator=(
        lazy_expression_materialization&&) = delete;

    ~lazy_expression_materialization() { release_tracking(); }

    [[nodiscard]] lazy_expression_facade view() const noexcept {
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

    lazy_expression_packed packed_;
    lazy_expression_allocation_statistics* stats_ = nullptr;
    std::uint64_t payload_bytes_ = 0;
    std::uint64_t admitted_bytes_ = 0;
};

struct lazy_expression_measurement {
    lazy_expression_size output_logical_length = 0;
    std::uint64_t logical_hash = 0;
    lazy_expression_allocation_statistics allocations{};
};

enum class lazy_expression_node_kind : std::uint8_t {
    slice,
    concat,
};

struct lazy_expression_node {
    lazy_expression_node_kind kind;
    lazy_expression_facade left;
    lazy_expression_facade right;
    lazy_expression_size slice_from;
    lazy_expression_size logical_size;
};

[[nodiscard]] lazy_expression_node make_lazy_expression_slice(
        lazy_expression_facade source,
        lazy_expression_size from,
        lazy_expression_size to) {
    if (from > to || to > source.bp_size()) {
        throw std::invalid_argument("invalid lazy-expression slice");
    }
    return lazy_expression_node{
        .kind = lazy_expression_node_kind::slice,
        .left = source,
        .right = source,
        .slice_from = from,
        .logical_size = to - from,
    };
}

[[nodiscard]] lazy_expression_node make_lazy_expression_concat(
        lazy_expression_facade left,
        lazy_expression_facade right) {
    constexpr lazy_expression_size max_size =
        (std::numeric_limits<lazy_expression_size>::max)();
    if (right.bp_size() > max_size - left.bp_size()) {
        throw std::overflow_error("lazy-expression concat length overflow");
    }
    return lazy_expression_node{
        .kind = lazy_expression_node_kind::concat,
        .left = left,
        .right = right,
        .slice_from = 0,
        .logical_size = left.bp_size() + right.bp_size(),
    };
}

[[nodiscard]] lazy_expression_size lazy_expression_node_bp_size(
        void const* opaque) noexcept {
    return static_cast<lazy_expression_node const*>(opaque)->logical_size;
}

[[nodiscard]] bool lazy_expression_node_worker_shareable(
        void const* opaque) noexcept {
    const auto& node = *static_cast<lazy_expression_node const*>(opaque);
    return lazy_is_worker_shareable(node.left)
        && lazy_is_worker_shareable(node.right);
}

[[nodiscard]] lazy_expression_facade::resident_probe_result
lazy_expression_node_resident_probe(
        void const*, lazy_expression_size, lazy_expression_size,
        std::uint8_t) noexcept {
    return std::nullopt;
}

void lazy_expression_copy_child(
        lazy_expression_facade child,
        lazy_expression_size from,
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
        lazy_expression_node const& node,
        lazy_expression_size from,
        std::span<std::uint8_t> output) {
    const lazy_expression_size count =
        static_cast<lazy_expression_size>(output.size());
    if (from > node.logical_size || count > node.logical_size - from) {
        throw std::invalid_argument("invalid lazy-expression read range");
    }
    if (output.empty()) {
        return;
    }
    if (node.kind == lazy_expression_node_kind::slice) {
        lazy_expression_copy_child(
            node.left, node.slice_from + from, output);
        return;
    }

    const lazy_expression_size left_size = node.left.bp_size();
    if (from >= left_size) {
        lazy_expression_copy_child(node.right, from - left_size, output);
        return;
    }
    const std::size_t left_count = static_cast<std::size_t>(
        std::min<lazy_expression_size>(count, left_size - from));
    lazy_expression_copy_child(node.left, from, output.first(left_count));
    if (left_count != output.size()) {
        lazy_expression_copy_child(
            node.right, 0, output.subspan(left_count));
    }
}

template <bool is_reversed, bool write_reversed>
[[nodiscard]] const std::uint8_t* lazy_expression_node_ptr_at(
        void const* opaque,
        lazy_expression_size from,
        std::size_t required_length,
        std::span<std::uint8_t> scratch) {
    const auto& node = *static_cast<lazy_expression_node const*>(opaque);
    const lazy_expression_size count =
        static_cast<lazy_expression_size>(required_length);
    if (required_length > scratch.size()
        || from > node.logical_size
        || count > node.logical_size - from) {
        throw std::invalid_argument("invalid lazy-expression facade read");
    }
    lazy_expression_size forward_from = from;
    if constexpr (is_reversed) {
        forward_from = node.logical_size - from - count;
    }
    std::span<std::uint8_t> output = scratch.first(required_length);
    lazy_expression_copy_forward(node, forward_from, output);
    if constexpr (is_reversed) {
        for (std::uint8_t& rank : output) {
            rank = lazy_expression_facade::traits::letter_complement(rank);
        }
    }
    if constexpr (is_reversed != write_reversed) {
        std::reverse(output.begin(), output.end());
    }
    return scratch.data();
}

const lazy_expression_facade::backend_ops lazy_expression_node_operations{
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

[[nodiscard]] lazy_expression_facade lazy_expression_view(
        lazy_expression_node const& node) noexcept {
    return lazy_expression_facade{
        std::addressof(node), std::addressof(lazy_expression_node_operations)};
}

[[nodiscard]] lazy_expression_materialization materialize_lazy_expression_view(
        lazy_expression_facade view,
        bool final_boundary,
        lazy_expression_allocation_statistics& stats) {
    auto& admission =
        bioseq::parallelism::MemoryAdmissionController::instance();
    const std::uint64_t admitted_bytes =
        bioseq::storage::packed_region_admission_bytes<seq::dna4>(
            view.bp_size(), lazy_expression_storage_width);
    const std::uint64_t reserved_before = admission.reserved_bytes();
    auto permit = admission.try_reserve(admitted_bytes);
    if (!permit.has_value()) {
        throw std::runtime_error(
            "memory admission rejected lazy-expression materialization");
    }
    lazy_expression_packed packed = bioseq::storage::materialize_packed_sequence(
        view, 0, view.bp_size(), lazy_expression_storage_width,
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
                / lazy_expression_storage_width) {
        throw std::overflow_error("lazy-expression payload byte overflow");
    }
    const std::uint64_t payload_bytes =
        static_cast<std::uint64_t>(descriptor.unit_count)
        * lazy_expression_storage_width;
    return lazy_expression_materialization{
        std::move(packed), stats, final_boundary, payload_bytes,
        observed_admitted_bytes};
}

[[nodiscard]] lazy_expression_measurement finish_lazy_expression_run(
        lazy_expression_materialization const& final_value) {
    const lazy_expression_facade final_view = final_value.view();
    return lazy_expression_measurement{
        .output_logical_length = final_view.bp_size(),
        .logical_hash = bioseq::storage::sequence_logical_hash_v1(
            final_view, lazy_expression_hash_seed),
    };
}

void validate_lazy_expression_run_released(
        lazy_expression_allocation_statistics const& stats,
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
    identifier_benchmark_measurement* out = nullptr;
    std::vector<std::string> scratch;   // simulates per-slot materialisation

    explicit bench_identifier_writer(std::size_t n, identifier_benchmark_measurement& o)
        : dest_n(n), out(&o), scratch(n) {}

    void begin_tuple() { for (auto& s : scratch) s.clear(); }
    void set_field(bioseq::io::identifier_parser::destination_slot slot,
                   const char* data, std::int32_t len) {
        auto& s = scratch.at(static_cast<std::size_t>(slot));
        s.assign(data, static_cast<std::size_t>(len));   // same shape as dumb copy
        ++out->parsed_field_count;
        out->parsed_field_bytes += s.size();
    }
    void set_null(bioseq::io::identifier_parser::destination_slot slot) {
        scratch.at(static_cast<std::size_t>(slot)).clear();
    }
    void end_tuple() {}
    void finish() {}
    [[nodiscard]] std::size_t destination_count() const noexcept { return dest_n; }
};
static_assert(bioseq::io::identifier_parser::identifier_destination_writer<bench_identifier_writer>);

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

[[nodiscard]] lazy_expression_input build_lazy_expression_input(
        std::string_view,
        std::string_view,
        simdjson::dom::element data,
        const run_constraints&) {
    const std::int64_t configured_logical_length = read_int_field(
        data, "bp_size", 8 * 1024 * 1024);
    if (configured_logical_length <= 0
        || configured_logical_length % 8 != 0
        || static_cast<std::uint64_t>(configured_logical_length)
            > (std::numeric_limits<std::size_t>::max)()) {
        throw std::invalid_argument(
            "lazy-expression bp_size must be a positive multiple of eight");
    }
    const std::size_t n = static_cast<std::size_t>(configured_logical_length);
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
            lazy_expression_hash_seed);

    return lazy_expression_input{
        .source = lazy_expression_range{source},
        .source_logical_length = static_cast<lazy_expression_size>(n),
        .expected_output_logical_length = static_cast<lazy_expression_size>(oracle.size()),
        .expected_hash = expected_hash,
    };
}

[[nodiscard]] run_constraints estimate_lazy_expression_resources(
        std::string_view,
        std::string_view,
        simdjson::dom::element data) noexcept {
    const std::int64_t configured_logical_length = read_int_field(
        data, "bp_size", 8 * 1024 * 1024);
    if (configured_logical_length <= 0) {
        return {};
    }
    const std::uint64_t bp = static_cast<std::uint64_t>(configured_logical_length);
    constexpr std::uint64_t max_size =
        (std::numeric_limits<std::size_t>::max)();
    const std::uint64_t reserved = bp > max_size / 2u
        ? max_size : bp * 2u;
    return run_constraints{
        .base_memory = static_cast<std::size_t>(reserved),
        .extra_memory = static_cast<std::size_t>(reserved),
    };
}

[[nodiscard]] lazy_expression_measurement run_lazy_expression_eager(
        const lazy_expression_input& input) {
    const lazy_expression_size n = input.source_logical_length;
    auto source_owner =
        bioseq::storage::make_resident_lazy_sequence(input.source);
    const lazy_expression_facade source =
        bioseq::storage::make_lazy_sequence_view(source_owner);
    lazy_expression_allocation_statistics stats{};
    const std::uint64_t reserved_baseline =
        bioseq::parallelism::MemoryAdmissionController::instance()
            .reserved_bytes();
    lazy_expression_measurement output{};
    {
        const lazy_expression_node a_node = make_lazy_expression_slice(
            source, n / 8u, 5u * n / 8u);
        const lazy_expression_facade a_expression = lazy_expression_view(a_node);
        const lazy_expression_materialization a = materialize_lazy_expression_view(
            a_expression, false, stats);

        const lazy_expression_node b_node = make_lazy_expression_slice(
            source, n / 2u, 7u * n / 8u);
        const lazy_expression_facade b_expression = lazy_expression_view(b_node);
        const lazy_expression_materialization b = materialize_lazy_expression_view(
            b_expression, false, stats);

        const lazy_expression_node c_node = make_lazy_expression_concat(
            a.view(), b.view());
        const lazy_expression_facade c_expression = lazy_expression_view(c_node);
        const lazy_expression_materialization c = materialize_lazy_expression_view(
            c_expression, false, stats);

        const lazy_expression_node d_node = make_lazy_expression_slice(
            c.view(), n / 4u, 3u * n / 4u);
        const lazy_expression_facade d_expression = lazy_expression_view(d_node);
        const lazy_expression_materialization d = materialize_lazy_expression_view(
            d_expression, false, stats);

        const lazy_expression_node e_node = make_lazy_expression_slice(
            a.view(), n / 8u, 3u * n / 8u);
        const lazy_expression_facade e_expression = lazy_expression_view(e_node);
        const lazy_expression_materialization e = materialize_lazy_expression_view(
            e_expression, false, stats);

        const lazy_expression_node result_node = make_lazy_expression_concat(
            d.view(), e.view());
        const lazy_expression_facade result_expression =
            lazy_expression_view(result_node);
        const lazy_expression_materialization result =
            materialize_lazy_expression_view(result_expression, true, stats);
        output = finish_lazy_expression_run(result);
    }
    validate_lazy_expression_run_released(stats, reserved_baseline);
    output.allocations = stats;
    return output;
}

[[nodiscard]] lazy_expression_measurement run_lazy_expression_deferred(
        const lazy_expression_input& input) {
    const lazy_expression_size n = input.source_logical_length;
    auto source_owner =
        bioseq::storage::make_resident_lazy_sequence(input.source);
    const lazy_expression_facade source =
        bioseq::storage::make_lazy_sequence_view(source_owner);
    lazy_expression_allocation_statistics stats{};
    const std::uint64_t reserved_baseline =
        bioseq::parallelism::MemoryAdmissionController::instance()
            .reserved_bytes();
    lazy_expression_measurement output{};
    {
        const lazy_expression_node a_node = make_lazy_expression_slice(
            source, n / 8u, 5u * n / 8u);
        const lazy_expression_facade a = lazy_expression_view(a_node);
        const lazy_expression_node b_node = make_lazy_expression_slice(
            source, n / 2u, 7u * n / 8u);
        const lazy_expression_facade b = lazy_expression_view(b_node);
        const lazy_expression_node c_node = make_lazy_expression_concat(a, b);
        const lazy_expression_facade c = lazy_expression_view(c_node);
        const lazy_expression_node d_node = make_lazy_expression_slice(
            c, n / 4u, 3u * n / 4u);
        const lazy_expression_facade d = lazy_expression_view(d_node);
        const lazy_expression_node e_node = make_lazy_expression_slice(
            a, n / 8u, 3u * n / 8u);
        const lazy_expression_facade e = lazy_expression_view(e_node);
        const lazy_expression_node result_node =
            make_lazy_expression_concat(d, e);
        const lazy_expression_facade result_expression =
            lazy_expression_view(result_node);
        const lazy_expression_materialization result =
            materialize_lazy_expression_view(result_expression, true, stats);
        output = finish_lazy_expression_run(result);
    }
    validate_lazy_expression_run_released(stats, reserved_baseline);
    output.allocations = stats;
    return output;
}

[[nodiscard]] lazy_expression_measurement run_lazy_expression(
        std::string_view algorithm,
        const parameter_map&,
        const lazy_expression_input& input) {
    if (algorithm == "bioseq_lazy_expr_eager_each_step") {
        return run_lazy_expression_eager(input);
    }
    if (algorithm == "bioseq_lazy_expr_deferred") {
        return run_lazy_expression_deferred(input);
    }
    throw std::invalid_argument("unknown lazy-expression benchmark algorithm");
}

[[nodiscard]] constexpr std::uint64_t lazy_expression_payload_bytes(
        lazy_expression_size bp) noexcept {
    constexpr std::uint64_t letters_per_unit = 32u;
    const std::uint64_t units = bp / letters_per_unit
        + static_cast<std::uint64_t>(bp % letters_per_unit != 0u);
    return units * lazy_expression_storage_width;
}

[[nodiscard]] std::uint64_t expected_lazy_expression_payload_bytes(
        bool eager,
        lazy_expression_size n) noexcept {
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
        lazy_expression_size n) {
    const auto admitted = [](lazy_expression_size bp) {
        return bioseq::storage::packed_region_admission_bytes<seq::dna4>(
            bp, lazy_expression_storage_width);
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
        const parameter_map&,
        const lazy_expression_input& input,
        const lazy_expression_measurement& output) {
    const bool eager = algorithm == "bioseq_lazy_expr_eager_each_step";
    const std::uint64_t expected_materializations = eager ? 6u : 1u;
    const std::uint64_t expected_intermediate = eager ? 5u : 0u;
    const std::uint64_t expected_payload =
        expected_lazy_expression_payload_bytes(eager, input.source_logical_length);
    const std::uint64_t expected_admitted =
        expected_lazy_expression_admitted_bytes(eager, input.source_logical_length);
    const auto& allocations = output.allocations;
    const bool correctness_ok =
        output.output_logical_length == input.expected_output_logical_length
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
    metrics.emplace("output_bp", static_cast<double>(output.output_logical_length));
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
        const lazy_expression_input&) noexcept {}

/// Allocate a unique scratch path under the system temp directory.
[[nodiscard]] std::string allocate_temp_path(std::string_view tag) {
    static std::atomic<std::uint64_t> counter{0};
    filesystem::path temp_directory_root = filesystem::temp_directory_path();
    filesystem::path full_path = temp_directory_root / fmt::format(
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
                                                  std::size_t mean_record_logical_length,
                                                  std::uint64_t seed,
                                                  std::string_view header_shape = "plain") {
    std::ofstream out(output_path, std::ios::binary);
    if (!out) {
        bioseq_log<logging_level::error>("[bioseq.io.benchmark] failed to open '{}' for writing", output_path);
        return 0;
    }
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int> base_dist(0, 3);
    std::normal_distribution<double> length_dist(
        static_cast<double>(mean_record_logical_length),
        static_cast<double>(mean_record_logical_length) * 0.1);
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
        // `header_shape` defaults to "plain" so every existing IO
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
struct corpus_descriptor {
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
[[nodiscard]] corpus_descriptor prepare_corpus(std::string_view data_dir,
                                                std::string_view test_case_name,
                                                simdjson::dom::element data) {
    corpus_descriptor descriptor;
    const std::string format_name = read_string_field(data, "format", "fasta");
    descriptor.is_fastq = (format_name == "fastq");
    descriptor.is_compressed = read_bool_field(data, "compressed", false);
    const std::string kind = read_string_field(data, "kind", "synthetic");

    if (kind == "real") {
        const std::string relative = read_string_field(data, "real_data_path", "");
        if (relative.empty()) {
            bioseq_log<logging_level::error>("[bioseq.io.benchmark] test case '{}' missing real_data_path",
                  test_case_name);
            return descriptor;
        }
        filesystem::path joined = filesystem::path{std::string{data_dir}} / relative;
        std::error_code ec;
        if (!filesystem::exists(joined, ec)) {
            bioseq_log<logging_level::error>("[bioseq.io.benchmark] real-data file missing for '{}': {}",
                  test_case_name, joined.string());
            return descriptor;
        }
        const auto observed_size = filesystem::file_size(joined, ec);
        if (ec) {
            bioseq_log<logging_level::error>("[bioseq.io.benchmark] cannot stat '{}': {}",
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
        bioseq_log<logging_level::error>("[bioseq.io.benchmark] test case '{}' has unknown kind '{}'",
              test_case_name, kind);
        return descriptor;
    }

    const std::size_t record_count = static_cast<std::size_t>(
        read_int_field(data, "record_count", 1000));
    const std::size_t mean_record_logical_length = static_cast<std::size_t>(
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
        record_count, mean_record_logical_length, seed, header_shape);
    if (bytes == 0) {
        bioseq_log<logging_level::error>("[bioseq.io.benchmark] synthetic corpus generation failed for '{}'",
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
/// record_count by mean_record_logical_length; for real-data corpora we
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
        filesystem::path joined = filesystem::path{std::string{data_dir}} / relative;
        std::error_code ec;
        const auto observed_size = filesystem::file_size(joined, ec);
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
    const auto mean_record_logical_length = static_cast<std::size_t>(
        read_int_field(data, "mean_record_length_bp", 200));
    return record_count * mean_record_logical_length;
}

/// Reader-side resource estimator. The reader input holds only the corpus path
/// plus a few size counters, so the persistent
/// (`base_memory`) cost is small.  However, we still budget `2x` the
/// total corpus bytes for both base + extra so the dynamic memory
/// budget throttles concurrent batches: the actual reader paths
/// (mmap'd regions, parallel-gzip ring buffers, decoded sequence
/// batches handed to the per-batch handler) push real RAM pressure
/// during `on_run`, even though the framework can't see those
/// allocations from the outside.
[[nodiscard]] run_constraints estimate_reader_resources(
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
/// `writer_input` slurps every input batch into RAM (see
/// `writer_input::batches`), so the persistent footprint *is* really
/// `~bytes` bytes.  Same `2x base + 2x extra` shape as the reader for
/// uniformity and headroom; callers can rely on it as a known
/// "this batch needs ~4x corpus bytes free" gate.  Compressed cells
/// are reader-only (`build_writer_input` early-returns) but we still
/// emit a sensible estimate so the framework doesn't think those
/// batches are free.
[[nodiscard]] run_constraints estimate_writer_resources(
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
// the io_measurement populated with observed_record_count + observed_body_bytes.

template <bool is_fasta>
[[nodiscard]] io_measurement run_bioseq_io_with_options(const std::string& path,
                                                    const reader_options& options) {
    using alphabet_config = benchmark_alphabet;
    using batch_t = sequence_batch<alphabet_config>;
    constexpr auto file_type = is_fasta
        ? sequence_file_format::fasta
        : sequence_file_format::fastq;

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
        bioseq_log<logging_level::error>("[bioseq.io.benchmark] read failed on '{}': {}", path, msg);
    };
    auto outcome = read_sequences_file<seq::dna4, file_type>(
        path, handler_fn, failure_fn, options);
    if (!outcome.has_value()) {
        bioseq_log<logging_level::error>("[bioseq.io.benchmark] read_sequences_file error: {}", outcome.error());
    }
    io_measurement out;
    out.observed_record_count = record_count_atomic.load(std::memory_order_relaxed);
    out.observed_body_bytes   = body_bytes_atomic.load(std::memory_order_relaxed);
    return out;
}

[[nodiscard]] io_measurement run_kseqpp(const std::string& path) {
    io_measurement out;
    klibpp::SeqStreamIn stream(path.c_str());
    klibpp::KSeq record;
    while (stream >> record) {
        out.observed_body_bytes += record.seq.size();
        out.observed_record_count += 1;
    }
    return out;
}

[[nodiscard]] io_measurement run_naive_ifstream_fasta(const std::string& path) {
    io_measurement out;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        bioseq_log<logging_level::error>("[bioseq.io.benchmark] naive ifstream open failed: '{}'", path);
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

[[nodiscard]] io_measurement run_naive_ifstream_fastq(const std::string& path) {
    io_measurement out;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        bioseq_log<logging_level::error>("[bioseq.io.benchmark] naive ifstream open failed: '{}'", path);
        return out;
    }
    std::string line;
    enum class fastq_step : std::uint8_t { header, sequence, separator, quality };
    fastq_step step = fastq_step::header;
    while (std::getline(in, line)) {
        switch (step) {
            case fastq_step::header:
                if (line.empty() || line.front() != '@') continue;
                step = fastq_step::sequence;
                break;
            case fastq_step::sequence: {
                std::size_t body_size = line.size();
                if (body_size > 0 && line[body_size - 1] == '\r') body_size -= 1;
                out.observed_body_bytes += body_size;
                step = fastq_step::separator;
                break;
            }
            case fastq_step::separator:
                step = fastq_step::quality;
                break;
            case fastq_step::quality:
                out.observed_record_count += 1;
                step = fastq_step::header;
                break;
        }
    }
    return out;
}

[[nodiscard]] io_measurement dispatch_reader(std::string_view algo,
                                         const reader_input& input) {
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
    bioseq_log<logging_level::error>("[bioseq.io.benchmark] unknown reader algorithm '{}'", algo);
    return {};
}

// =====================================================================
//  Reader benchmark lambdas
// =====================================================================

[[nodiscard]] reader_input build_reader_input(std::string_view data_dir,
                                                std::string_view test_case_name,
                                                simdjson::dom::element data,
                                                const run_constraints& /*constraints*/) {
    const corpus_descriptor descriptor =
        prepare_corpus(data_dir, test_case_name, data);
    reader_input input;
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
    bioseq_log<logging_level::debug>("[bioseq.io.benchmark] reader corpus ready: '{}' (path={}, bytes={})",
            test_case_name, input.corpus_path, input.bytes_on_disk);
    return input;
}

[[nodiscard]] io_measurement run_reader(std::string_view algo,
                                    const parameter_map& /*params*/,
                                    const reader_input& input) {
    if (input.corpus_path.empty()) return {};
    return dispatch_reader(algo, input);
}

[[nodiscard]] std::map<std::string, double> evaluate_reader(
    std::string_view /*algo*/,
    const parameter_map& /*params*/,
    const reader_input& input,
    const io_measurement& output)
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
                    const reader_input& input)
{
    if (!input.is_synthetic || input.corpus_path.empty()) return;
    std::error_code ec;
    filesystem::remove(input.corpus_path, ec);
}

// =====================================================================
// Identifier-parser overhead benchmark
// =====================================================================

[[nodiscard]] identifier_benchmark_input build_identifier_input(
        std::string_view data_dir, std::string_view test_case_name,
        simdjson::dom::element data, const run_constraints& constraints) {
    identifier_benchmark_input input;
    input.reader = build_reader_input(data_dir, test_case_name, data, constraints);
    if (input.reader.corpus_path.empty()) return input;

    using namespace bioseq::io::identifier_parser;
    const std::vector<column_descriptor> columns{
        {.name="header",              .is_text=true,  .is_primary_key=false, .not_null=false, .has_default=false, .is_identity_or_generated=false},
        {.name="accession",           .is_text=true,  .is_primary_key=false, .not_null=false, .has_default=false, .is_identity_or_generated=false},
        {.name="organism_name",       .is_text=true,  .is_primary_key=false, .not_null=false, .has_default=false, .is_identity_or_generated=false},
        {.name="organism_identifier", .is_text=false, .is_primary_key=false, .not_null=false, .has_default=false, .is_identity_or_generated=false},
        {.name="protein_existence",   .is_text=false, .is_primary_key=false, .not_null=false, .has_default=false, .is_identity_or_generated=false},
        {.name="sequence_version",    .is_text=false, .is_primary_key=false, .not_null=false, .has_default=false, .is_identity_or_generated=false},
    };
    auto map = resolve_identifier_mapping(
        header_format::uniprotkb, header_format::uniprotkb, columns,
        std::span<const user_column_mapping>{});
    if (!map) {
        bioseq_log<logging_level::error>("[bioseq.io.benchmark] identifier mapping failed: {}", map.error());
        input.reader.corpus_path.clear();
        return input;
    }
    input.mapping = std::move(*map);
    return input;
}

[[nodiscard]] identifier_benchmark_measurement run_identifier_bench(
        std::string_view algo, const parameter_map& /*params*/,
        const identifier_benchmark_input& input) {
    identifier_benchmark_measurement out;
    if (input.reader.corpus_path.empty()) return out;

    using namespace bioseq::io;
    using namespace bioseq::io::identifier_parser;
    using alphabet_config = benchmark_alphabet;
    using batch_t = sequence_batch<alphabet_config>;

    std::string dumb_header_copy;
    identifier_parse_state parse_state;
    bench_identifier_writer parsed_writer{input.mapping.routing.destination_count, out};
    const auto writer_ref = make_identifier_writer_reference(parsed_writer);

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
                    header_format::uniprotkb, header,
                    input.mapping.routing, parse_state, writer_ref);
                if (!parsed)
                    bioseq_log<logging_level::error>("[bioseq.io.benchmark] identifier parse failed: {}", parsed.error());
                parsed_writer.end_tuple();
            } else {  // bioseq_dumb_header_fasta
                dumb_header_copy.assign(header.data(), header.size());
            }
        }
    };
    auto failure_fn = [](std::string&& msg) {
        bioseq_log<logging_level::error>("[bioseq.io.benchmark] identifier benchmark read failed: {}", msg);
    };
    reader_options opts{};
    opts.io.gzip_auto_detect = false;
    opts.io.prefer_mmap = false;
    opts.concurrency.worker_count = 0;
    (void) read_sequences_file<seq::dna4, sequence_file_format::fasta>(
        input.reader.corpus_path, handler_fn, failure_fn, opts);
    return out;
}

[[nodiscard]] std::map<std::string, double> evaluate_identifier_bench(
        std::string_view /*algo*/, const parameter_map& /*params*/,
        const identifier_benchmark_input& input, const identifier_benchmark_measurement& out) {
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
                              const identifier_benchmark_input& input) {
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
template <bool is_fastq>
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
                       + (is_fastq ? quality.size() : 0) + 2);
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
    if constexpr (is_fastq) {
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
template <bool is_fastq>
[[nodiscard]] std::uint64_t hash_batches_xor(
    const std::vector<benchmark_batch>& batches)
{
    std::uint64_t accumulator = 0;
    for (const auto& batch : batches) {
        const std::size_t record_count_in_batch = batch.size();
        for (std::size_t record_index = 0;
             record_index < record_count_in_batch; ++record_index) {
            const auto seq_view = batch.sequence(record_index);
            const auto header_view = batch.header(record_index);
            std::span<const std::uint8_t> qual_view{};
            if constexpr (is_fastq) {
                qual_view = batch.quality(record_index);
            }
            accumulator ^= hash_record_xor<is_fastq>(
                header_view, seq_view, qual_view);
        }
    }
    return accumulator;
}

/// Slurp `path` into an in-memory list of batches via the bioseq reader.
template <bool is_fasta>
[[nodiscard]] bool slurp_corpus(const std::string& path,
                                  std::vector<benchmark_batch>& out_batches,
                                  std::size_t& out_record_count) {
    constexpr auto file_type = is_fasta
        ? sequence_file_format::fasta
        : sequence_file_format::fastq;

    out_batches.clear();
    out_record_count = 0;

    auto handler_fn = [&](benchmark_batch&& batch) {
        out_record_count += batch.size();
        out_batches.push_back(std::move(batch));
    };
    auto failure_fn = [&path](std::string&& msg) {
        bioseq_log<logging_level::error>("[bioseq.io.benchmark] slurp failed on '{}': {}", path, msg);
    };
    reader_options options{};
    options.io.gzip_auto_detect = false;
    options.io.prefer_mmap      = true;
    options.concurrency.worker_count = 0;

    auto outcome = read_sequences_file<seq::dna4, file_type>(
        path, handler_fn, failure_fn, options);
    if (!outcome.has_value()) {
        bioseq_log<logging_level::error>("[bioseq.io.benchmark] slurp_corpus read error: {}", outcome.error());
        return false;
    }
    return !out_batches.empty();
}

[[nodiscard]] writer_input build_writer_input(std::string_view data_dir,
                                                std::string_view test_case_name,
                                                simdjson::dom::element data,
                                                const run_constraints& /*constraints*/) {
    writer_input input;

    // The .fastq.gz cell is reader-only; writers don't have a compress
    // path. Skip silently.
    const bool compressed_hint = read_bool_field(data, "compressed", false);
    if (compressed_hint) {
        bioseq_log<logging_level::debug>("[bioseq.io.benchmark] writer skipping gz cell '{}'", test_case_name);
        return input;
    }
    const corpus_descriptor descriptor =
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
        bioseq_log<logging_level::error>("[bioseq.io.benchmark] writer slurp failed for '{}' — clearing input",
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
    bioseq_log<logging_level::debug>("[bioseq.io.benchmark] writer corpus ready: '{}' (records={}, scratch={})",
            test_case_name, input.source_record_count, input.output_scratch_path);
    return input;
}

template <bool is_fasta>
[[nodiscard]] io_measurement run_bioseq_writer(const writer_input& input,
                                           std::size_t thread_count,
                                           bool use_mmap_sink = false) {
    constexpr auto file_type = is_fasta
        ? sequence_file_format::fasta
        : sequence_file_format::fastq;
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
        bioseq_log<logging_level::error>("[bioseq.io.benchmark] writer open failed: {}", writer_or_err.error());
        return {};
    }
    auto& writer = *writer_or_err;
    const std::size_t batches_total = input.batches.size();
    if (thread_count <= 1 || batches_total <= 1) {
        for (const auto& batch : input.batches) {
            if (!writer.write_batch(batch)) {
                bioseq_log<logging_level::error>("[bioseq.io.benchmark] writer write_batch failed");
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
        bioseq_log<logging_level::error>("[bioseq.io.benchmark] writer close failed: {}", close_outcome.error());
        return {};
    }
    io_measurement out;
    out.observed_record_count = input.source_record_count;
    std::error_code ec;
    out.bytes_written = filesystem::file_size(input.output_scratch_path, ec);
    return out;
}

template <bool is_fasta>
[[nodiscard]] io_measurement run_naive_ofstream(const writer_input& input) {
    std::ofstream out(input.output_scratch_path, std::ios::binary);
    if (!out) {
        bioseq_log<logging_level::error>("[bioseq.io.benchmark] naive ofstream open failed: '{}'",
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
            scratch.push_back(is_fasta ? '>' : '@');
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
                // any alphabet supported by `benchmark_alphabet`.
                scratch.push_back(seq_traits::letter_to_char(rank));
                if constexpr (is_fasta) {
                    ++in_line_count;
                    if (in_line_count == fasta_wrap &&
                        letter_index + 1 < total_letters) {
                        scratch.push_back('\n');
                        in_line_count = 0;
                    }
                }
            }
            if constexpr (!is_fasta) {
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
    io_measurement result;
    result.observed_record_count = input.source_record_count;
    std::error_code ec;
    result.bytes_written = filesystem::file_size(input.output_scratch_path, ec);
    return result;
}

[[nodiscard]] io_measurement dispatch_writer(std::string_view algo,
                                         const writer_input& input) {
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
    bioseq_log<logging_level::error>("[bioseq.io.benchmark] unknown writer algorithm '{}'", algo);
    return {};
}

[[nodiscard]] io_measurement run_writer(std::string_view algo,
                                    const parameter_map& /*params*/,
                                    const writer_input& input) {
    if (input.batches.empty() || input.output_scratch_path.empty()) return {};
    return dispatch_writer(algo, input);
}

/// Re-read the writer's output to count records AND verify byte-faithful
/// content. Returns 1.0 only if both:
///   1. record count matches the input slurp;
///   2. the XOR-aggregated content hash matches the input slurp's hash.
/// Either failure returns 0.0 — the bench then surfaces it as a
/// roundtrip violation. This catches silent corruption that a
/// count-only check misses (e.g. truncated quality in the naive
/// writer, headers split from bodies by mid-record flushes).
template <bool is_fasta>
[[nodiscard]] double roundtrip_match(const std::string& output_path,
                                       std::size_t expected_record_count,
                                       std::uint64_t expected_content_hash) {
    if (output_path.empty()) return 0.0;
    std::error_code ec;
    if (!filesystem::exists(output_path, ec) || filesystem::file_size(output_path, ec) == 0) {
        return 0.0;
    }
    constexpr auto file_type = is_fasta
        ? sequence_file_format::fasta
        : sequence_file_format::fastq;
    std::atomic<std::size_t> record_count{0};
    std::atomic<std::uint64_t> content_hash{0};
    auto handler_fn = [&record_count, &content_hash](benchmark_batch&& batch) {
        record_count.fetch_add(batch.size(), std::memory_order_relaxed);
        // Hash this batch's contribution and XOR into the global
        // accumulator. XOR is associative + commutative, so concurrent
        // batch handlers contribute correctly via fetch_xor.
        const std::uint64_t batch_hash = hash_batches_xor<!is_fasta>(
            std::vector<benchmark_batch>{std::move(batch)});
        content_hash.fetch_xor(batch_hash, std::memory_order_relaxed);
    };
    auto failure_fn = [&output_path](std::string&& msg) {
        bioseq_log<logging_level::error>("[bioseq.io.benchmark] roundtrip read failed on '{}': {}",
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
        bioseq_log<logging_level::error>("[bioseq.io.benchmark] roundtrip record count mismatch on '{}': "
             "expected={}, observed={}",
             output_path, expected_record_count, observed_records);
        return 0.0;
    }
    if (observed_hash != expected_content_hash) {
        bioseq_log<logging_level::error>("[bioseq.io.benchmark] roundtrip content hash mismatch on '{}': "
             "expected=0x{:016x}, observed=0x{:016x}",
             output_path, expected_content_hash, observed_hash);
        return 0.0;
    }
    return 1.0;
}

[[nodiscard]] std::map<std::string, double> evaluate_writer(
    std::string_view /*algo*/,
    const parameter_map& /*params*/,
    const writer_input& input,
    const io_measurement& output)
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
                    const writer_input& input)
{
    std::error_code ec;
    if (input.is_synthetic_source && !input.source_corpus_path.empty()) {
        filesystem::remove(input.source_corpus_path, ec);
    }
    if (!input.output_scratch_path.empty()) {
        filesystem::remove(input.output_scratch_path, ec);
    }
}

}  // namespace

export namespace bioseq::io::benchmark {

/// Run every sequence IO benchmark selected by the configuration.
int run(int argc, char** argv) {
    configure_logging(true);
    fmtlog::setLogLevel(fmtlog::INF);

    const std::string config_path{BIOSEQ_IO_BENCH_CONFIG_PATH};
    const std::string report_dir{BIOSEQ_IO_REPORT_DIR};
    const std::string data_dir{BIOSEQ_IO_DATA_DIR};

    using bioseq::bench::framework::create_benchmark;

    using resource_estimator = std::function<run_constraints(std::string_view,
                                                    std::string_view,
                                                    simdjson::dom::element)>;
    using reader_builder = std::function<reader_input(std::string_view,
                                                    std::string_view,
                                                    simdjson::dom::element,
                                                    const run_constraints&)>;
    using reader_runner = std::function<io_measurement(std::string_view,
                                                const parameter_map&,
                                                const reader_input&)>;
    using reader_evaluator = std::function<std::map<std::string, double>(
        std::string_view, const parameter_map&,
        const reader_input&, const io_measurement&)>;
    using reader_cleanup = std::function<void(std::string_view,
                                                std::string_view,
                                                simdjson::dom::element,
                                                const reader_input&)>;

    using writer_builder = std::function<writer_input(std::string_view,
                                                    std::string_view,
                                                    simdjson::dom::element,
                                                    const run_constraints&)>;
    using writer_runner = std::function<io_measurement(std::string_view,
                                                const parameter_map&,
                                                const writer_input&)>;
    using writer_evaluator = std::function<std::map<std::string, double>(
        std::string_view, const parameter_map&,
        const writer_input&, const io_measurement&)>;
    using writer_cleanup = std::function<void(std::string_view,
                                                std::string_view,
                                                simdjson::dom::element,
                                                const writer_input&)>;

    // Identifier-parser overhead benchmark function types.
    using identifier_builder = std::function<identifier_benchmark_input(
        std::string_view, std::string_view, simdjson::dom::element,
        const run_constraints&)>;
    using identifier_runner = std::function<identifier_benchmark_measurement(
        std::string_view, const parameter_map&, const identifier_benchmark_input&)>;
    using identifier_evaluator = std::function<std::map<std::string, double>(
        std::string_view, const parameter_map&,
        const identifier_benchmark_input&, const identifier_benchmark_measurement&)>;
    using identifier_cleanup = std::function<void(
        std::string_view, std::string_view, simdjson::dom::element,
        const identifier_benchmark_input&)>;

    using lazy_expression_builder = std::function<lazy_expression_input(
        std::string_view, std::string_view, simdjson::dom::element,
        const run_constraints&)>;
    using lazy_expression_runner = std::function<lazy_expression_measurement(
        std::string_view, const parameter_map&, const lazy_expression_input&)>;
    using lazy_expression_evaluator = std::function<std::map<std::string, double>(
        std::string_view, const parameter_map&,
        const lazy_expression_input&, const lazy_expression_measurement&)>;
    using lazy_expression_cleanup = std::function<void(
        std::string_view, std::string_view, simdjson::dom::element,
        const lazy_expression_input&)>;

    return bioseq::bench::framework::run(
        argc, argv, report_dir, data_dir,
        create_benchmark<reader_input, io_measurement>(
            "IO readers — FASTA (plain / async / mmap)",
            "bench_io_readers_fasta", config_path,
            resource_estimator{estimate_reader_resources},
            reader_builder{build_reader_input},
            reader_runner{run_reader},
            reader_evaluator{evaluate_reader},
            reader_cleanup{cleanup_reader}),
        create_benchmark<reader_input, io_measurement>(
            "IO readers — FASTQ (plain / async / mmap)",
            "bench_io_readers_fastq", config_path,
            resource_estimator{estimate_reader_resources},
            reader_builder{build_reader_input},
            reader_runner{run_reader},
            reader_evaluator{evaluate_reader},
            reader_cleanup{cleanup_reader}),
        create_benchmark<reader_input, io_measurement>(
            "IO readers — gzipped paths",
            "bench_io_readers_gz", config_path,
            resource_estimator{estimate_reader_resources},
            reader_builder{build_reader_input},
            reader_runner{run_reader},
            reader_evaluator{evaluate_reader},
            reader_cleanup{cleanup_reader}),
        create_benchmark<writer_input, io_measurement>(
            "IO writers — FASTA (sync / parallel)",
            "bench_io_writers_fasta", config_path,
            resource_estimator{estimate_writer_resources},
            writer_builder{build_writer_input},
            writer_runner{run_writer},
            writer_evaluator{evaluate_writer},
            writer_cleanup{cleanup_writer}),
        create_benchmark<writer_input, io_measurement>(
            "IO writers — FASTQ (sync / parallel)",
            "bench_io_writers_fastq", config_path,
            resource_estimator{estimate_writer_resources},
            writer_builder{build_writer_input},
            writer_runner{run_writer},
            writer_evaluator{evaluate_writer},
            writer_cleanup{cleanup_writer}),
        create_benchmark<identifier_benchmark_input, identifier_benchmark_measurement>(
            "IO identifier parser — UniProt header overhead",
            "bench_io_identifier_parser", config_path,
            resource_estimator{estimate_reader_resources},
            identifier_builder{build_identifier_input},
            identifier_runner{run_identifier_bench},
            identifier_evaluator{evaluate_identifier_bench},
            identifier_cleanup{cleanup_identifier_bench}),
        create_benchmark<lazy_expression_input, lazy_expression_measurement>(
            "Lazy sequence expression — eager versus deferred materialization",
            "bench_io_lazy_sequence_expression", config_path,
            resource_estimator{estimate_lazy_expression_resources},
            lazy_expression_builder{build_lazy_expression_input},
            lazy_expression_runner{run_lazy_expression},
            lazy_expression_evaluator{evaluate_lazy_expression},
            lazy_expression_cleanup{cleanup_lazy_expression}));
}

}  // namespace bioseq::io::benchmark
