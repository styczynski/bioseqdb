/**
 * @file lazy_expression.cppm
 * @brief Lazy slicing and concatenation benchmark workload.
 */
module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <libassert/assert.hpp>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <random>
#include <optional>
#include <simdjson.h>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module bioseq.io.benchmark:lazy_expression;

import :common;
import bioseq.common.seqan_adapter;
import bioseq.sequences;
import bioseq.sequences.cigar;
import bioseq.common.context;
import bioseq.simd.sorting;
import bioseq.storage.batch_sort;
import bioseq.instrumentation.clock;
import bioseq.storage.lazy_sequence;
import bioseq.storage.size;
import bioseq.parallelism.memory_admission;

namespace bioseq::io::benchmark::detail {

/// Own a packed result and track its allocated bytes and memory reservation.
class lazy_expression_materialization {
  public:
    /// Take ownership of a packed result and add its sizes to the run statistics.
    lazy_expression_materialization(lazy_expression_packed&& packed,
                                    lazy_expression_allocation_statistics& stats,
                                    bool final_boundary,
                                    std::uint64_t payload_bytes,
                                    std::uint64_t observed_admitted_bytes) noexcept
        : packed_(std::move(packed)), stats_(std::addressof(stats)), payload_bytes_(payload_bytes),
          admitted_bytes_(observed_admitted_bytes) {
        ++stats.payload_materializations;
        if (final_boundary) {
            ++stats.boundary_materializations;
        } else {
            ++stats.intermediate_payload_materializations;
        }
        stats.payload_bytes_materialized += payload_bytes_;
        stats.live_payload_bytes += payload_bytes_;
        stats.peak_live_payload_bytes =
            std::max(stats.peak_live_payload_bytes, stats.live_payload_bytes);
        stats.admitted_bytes_total += admitted_bytes_;
        stats.live_admitted_bytes += admitted_bytes_;
        stats.peak_live_admitted_bytes =
            std::max(stats.peak_live_admitted_bytes, stats.live_admitted_bytes);
    }

    /// Move the result and transfer responsibility for updating the statistics
    lazy_expression_materialization(lazy_expression_materialization&& other) noexcept
        : packed_(std::move(other.packed_)), stats_(std::exchange(other.stats_, nullptr)),
          payload_bytes_(other.payload_bytes_), admitted_bytes_(other.admitted_bytes_) {}

    /// Copying would count the same owned result  twice.
    lazy_expression_materialization(lazy_expression_materialization const&) = delete;

    /// Copy assignment would duplicate ownership of a result.
    lazy_expression_materialization& operator=(lazy_expression_materialization const&) = delete;

    /// Move assignment is disabled to keep each result's accounting unchanged.
    lazy_expression_materialization& operator=(lazy_expression_materialization&&) = delete;

    /// Remove the result from the live counts before releasing its storage.
    ~lazy_expression_materialization() {
        release_tracking();
    }

    /// Return a sequence view that borrows this result's storage.
    [[nodiscard]] lazy_expression_facade view() const noexcept {
        return packed_.view();
    }

  private:
    /// Subtract this result from the live byte counts once.
    void release_tracking() noexcept {
        if (stats_ == nullptr) {
            return;
        }
        DEBUG_ASSERT(stats_->live_payload_bytes >= payload_bytes_,
                     "lazy-expression live payload accounting underflow");
        DEBUG_ASSERT(stats_->live_admitted_bytes >= admitted_bytes_,
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

/// Operations supported by, the benchmark's expression nodes.
enum class lazy_expression_node_kind : std::uint8_t {
    slice,
    concat,
};

/// A slice or concatenation that borrows its child sequences.
/// Reading a node follows those children without storing an intermediate  result.
struct lazy_expression_node {
    lazy_expression_node_kind kind;
    lazy_expression_facade left;
    lazy_expression_facade right;
    lazy_expression_size slice_from;
    lazy_expression_size logical_size;
};

/// Validate bounds and describe a non-owning slice of one expression.
[[nodiscard]] lazy_expression_node make_lazy_expression_slice(lazy_expression_facade source,
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

/// Describe a concatenation, rejecting a combined length that would overfxow.
[[nodiscard]] lazy_expression_node make_lazy_expression_concat(lazy_expression_facade left,
                                                               lazy_expression_facade right) {
    constexpr lazy_expression_size max_size = (std::numeric_limits<lazy_expression_size>::max)();
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

/// Return the node's sequence length through the backend callback.
[[nodiscard]] lazy_expression_size lazy_expression_node_bp_size(void const* opaque) noexcept {
    return static_cast<lazy_expression_node const*>(opaque)->logical_size;
}

/// Return true when both child views may be shared with workers.
[[nodiscard]] bool lazy_expression_node_worker_shareable(void const* opaque) noexcept {
    const auto& node = *static_cast<lazy_expression_node const*>(opaque);
    return bioseq::storage::is_worker_shareable(node.left) &&
           bioseq::storage::is_worker_shareable(node.right);
}

/// Report no stored packed result so reads must evaluate the expression.
[[nodiscard]] lazy_expression_facade::resident_probe_result lazy_expression_node_resident_probe(
    void const*, lazy_expression_size, lazy_expression_size, std::uint8_t) noexcept {
    return std::nullopt;
}

/// Copy a child range into the supplied rank buffer, checking coordinate overflow.
void lazy_expression_copy_child(lazy_expression_facade child,
                                lazy_expression_size from,
                                std::span<std::uint8_t> output) {
    constexpr auto max_size = (std::numeric_limits<std::size_t>::max)();
    if (from > max_size || output.size() > max_size - from) {
        throw std::overflow_error("lazy-expression coordinate overflow");
    }
    const std::size_t begin = static_cast<std::size_t>(from);
    child.template copy_ranks<false, false>(output, begin, begin + output.size());
}

/// Read a node range by following its slice offset or concatenation children.
void lazy_expression_copy_forward(lazy_expression_node const& node,
                                  lazy_expression_size from,
                                  std::span<std::uint8_t> output) {
    const lazy_expression_size count = static_cast<lazy_expression_size>(output.size());
    if (from > node.logical_size || count > node.logical_size - from) {
        throw std::invalid_argument("invalid lazy-expression read range");
    }
    if (output.empty()) {
        return;
    }
    if (node.kind == lazy_expression_node_kind::slice) {
        lazy_expression_copy_child(node.left, node.slice_from + from, output);
        return;
    }

    const lazy_expression_size left_size = node.left.bp_size();
    if (from >= left_size) {
        lazy_expression_copy_child(node.right, from - left_size, output);
        return;
    }
    const std::size_t left_count =
        static_cast<std::size_t>(std::min<lazy_expression_size>(count, left_size - from));
    lazy_expression_copy_child(node.left, from, output.first(left_count));
    if (left_count != output.size()) {
        lazy_expression_copy_child(node.right, 0, output.subspan(left_count));
    }
}

/// Copy a node range into scratch memory in  the requested orientation.
/// Reverse reads also complement the DNA ranks.
template <bool p_is_reversed, bool p_write_reversed>
[[nodiscard]] const std::uint8_t* lazy_expression_node_ptr_at(void const* opaque,
                                                              lazy_expression_size from,
                                                              std::size_t required_length,
                                                              std::span<std::uint8_t> scratch) {
    const auto& node = *static_cast<lazy_expression_node const*>(opaque);
    const lazy_expression_size count = static_cast<lazy_expression_size>(required_length);
    if (required_length > scratch.size() || from > node.logical_size ||
        count > node.logical_size - from) {
        throw std::invalid_argument("invalid lazy-expression facade read");
    }
    lazy_expression_size forward_from = from;
    if constexpr (p_is_reversed) {
        forward_from = node.logical_size - from - count;
    }
    std::span<std::uint8_t> output = scratch.first(required_length);
    lazy_expression_copy_forward(node, forward_from, output);
    if constexpr (p_is_reversed) {
        for (std::uint8_t& rank : output) {
            rank = lazy_expression_facade::traits::letter_complement(rank);
        }
    }
    if constexpr (p_is_reversed != p_write_reversed) {
        std::reverse(output.begin(), output.end());
    }
    return scratch.data();
}

const lazy_expression_facade::backend_ops lazy_expression_node_operations{
    .bp_size = &lazy_expression_node_bp_size,
    .ptr_at =
        {
            &lazy_expression_node_ptr_at<false, false>,
            &lazy_expression_node_ptr_at<false, true>,
            &lazy_expression_node_ptr_at<true, false>,
            &lazy_expression_node_ptr_at<true, true>,
        },
    .try_resident_packed = &lazy_expression_node_resident_probe,
    .copy_full_packed = nullptr,
    .worker_shareable = &lazy_expression_node_worker_shareable,
};

/// Expose a node through the lazy-sequence interface. The node must outlive the view.
[[nodiscard]] lazy_expression_facade
lazy_expression_view(lazy_expression_node const& node) noexcept {
    return lazy_expression_facade{std::addressof(node),
                                  std::addressof(lazy_expression_node_operations)};
}

/// Reserve memory and copy a view into packed storage Track the allocation
/// and check that the memory controller records the expected reservation.
[[nodiscard]] lazy_expression_materialization
materialize_lazy_expression_view(lazy_expression_facade view,
                                 bool final_boundary,
                                 lazy_expression_allocation_statistics& stats) {
    auto& admission = bioseq::parallelism::memory_admission_controller::instance();
    const std::uint64_t admitted_bytes = bioseq::storage::packed_region_admission_bytes<seq::dna4>(
        view.bp_size(), lazy_expression_storage_width);
    const std::uint64_t reserved_before = admission.reserved_bytes();
    auto permit = admission.try_reserve(admitted_bytes);
    if (!permit.has_value()) {
        throw std::runtime_error("memory admission rejected lazy-expression materialization");
    }
    lazy_expression_packed packed = bioseq::storage::materialize_packed_sequence(
        view, 0, view.bp_size(), lazy_expression_storage_width, std::move(*permit));
    const std::uint64_t reserved_after = admission.reserved_bytes();
    if (reserved_after < reserved_before || reserved_after - reserved_before != admitted_bytes) {
        throw std::runtime_error("lazy-expression observed admission delta mismatch");
    }
    const std::uint64_t observed_admitted_bytes = reserved_after - reserved_before;
    const auto& descriptor = packed.descriptor();
    if (descriptor.unit_count >
        (std::numeric_limits<std::uint64_t>::max)() / lazy_expression_storage_width) {
        throw std::overflow_error("lazy-expression payload byte overflow");
    }
    const std::uint64_t payload_bytes =
        static_cast<std::uint64_t>(descriptor.unit_count) * lazy_expression_storage_width;
    return lazy_expression_materialization{
        std::move(packed), stats, final_boundary, payload_bytes, observed_admitted_bytes};
}

/// Compute the logical output length and deterministic content hash.
[[nodiscard]] lazy_expression_measurement
finish_lazy_expression_run(lazy_expression_materialization const& final_value) {
    const lazy_expression_facade final_view = final_value.view();
    return lazy_expression_measurement{
        .output_logical_length = final_view.bp_size(),
        .logical_hash =
            bioseq::storage::hash_logical_sequence(final_view, lazy_expression_hash_seed),
    };
}

/// Check that all result storage and memory reservations were released.
void validate_lazy_expression_run_released(lazy_expression_allocation_statistics const& stats,
                                           std::uint64_t reserved_baseline) {
    const std::uint64_t reserved_after =
        bioseq::parallelism::memory_admission_controller::instance().reserved_bytes();
    if (stats.live_payload_bytes != 0u || stats.live_admitted_bytes != 0u ||
        reserved_after != reserved_baseline) {
        throw std::runtime_error(
            "lazy-expression materialization ownership did not return to baseline");
    }
}

/// Generate the deterministic DNA4 source used by both algorithms.
[[nodiscard]] std::vector<seq::dna4> make_lazy_expression_source(std::size_t bp_size,
                                                                 std::uint64_t seed) {
    std::vector<seq::dna4> source(bp_size);
    std::uint64_t state = seed;
    for (seq::dna4& letter : source) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        letter.assign_rank(static_cast<seqan3::alphabet_rank_t<seq::dna4>>((state >> 33u) & 0x3u));
    }
    return source;
}

/// Append source ranks directly to the expected result.
void append_lazy_expression_oracle_segment(std::vector<std::uint8_t>& oracle,
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

/// Build the source and independent expected result for one test case.
[[nodiscard]] lazy_expression_input build_lazy_expression_input(std::string_view,
                                                                std::string_view,
                                                                simdjson::dom::element data,
                                                                const run_constraints&) {
    const std::int64_t configured_logical_length = read_int_field(data, "bp_size", 8 * 1024 * 1024);
    if (configured_logical_length <= 0 || configured_logical_length % 8 != 0 ||
        static_cast<std::uint64_t>(configured_logical_length) >
            (std::numeric_limits<std::size_t>::max)()) {
        throw std::invalid_argument("lazy-expression bp_size must be a positive multiple of eight");
    }
    const std::size_t logical_length = static_cast<std::size_t>(configured_logical_length);
    const std::uint64_t seed = static_cast<std::uint64_t>(read_int_field(data, "seed", 377));
    std::vector<seq::dna4> source = make_lazy_expression_source(logical_length, seed);

    // Build the expected result directly from  source intervals:
    //   concat(concat(source[3N/8:5N/8], source[N/2:3N/4]),
    //          source[N/4:N/2]).
    // This checks the expression without using its slice or concateation code.
    std::vector<std::uint8_t> oracle;
    oracle.reserve(3u * logical_length / 4u);
    const std::span<const seq::dna4> source_span{source};
    append_lazy_expression_oracle_segment(
        oracle, source_span, 3u * logical_length / 8u, 5u * logical_length / 8u);
    append_lazy_expression_oracle_segment(
        oracle, source_span, logical_length / 2u, 3u * logical_length / 4u);
    append_lazy_expression_oracle_segment(
        oracle, source_span, logical_length / 4u, logical_length / 2u);
    const std::uint64_t expected_hash = bioseq::storage::hash_logical_ranks(
        oracle, static_cast<std::uint64_t>(oracle.size()), lazy_expression_hash_seed);

    return lazy_expression_input{
        .source = lazy_expression_range{source},
        .source_logical_length = static_cast<lazy_expression_size>(logical_length),
        .expected_output_logical_length = static_cast<lazy_expression_size>(oracle.size()),
        .expected_hash = expected_hash,
    };
}

/// Estimate memory for source data, the expected result, and copied sequences.
[[nodiscard]] run_constraints estimate_lazy_expression_resources(
    std::string_view, std::string_view, simdjson::dom::element data) noexcept {
    const std::int64_t configured_logical_length = read_int_field(data, "bp_size", 8 * 1024 * 1024);
    if (configured_logical_length <= 0) {
        return {};
    }
    const std::uint64_t bp = static_cast<std::uint64_t>(configured_logical_length);
    constexpr std::uint64_t max_size = (std::numeric_limits<std::size_t>::max)();
    const std::uint64_t reserved = bp > max_size / 2u ? max_size : bp * 2u;
    return run_constraints{
        .base_memory = static_cast<std::size_t>(reserved),
        .extra_memory = static_cast<std::size_t>(reserved),
    };
}

/// Copy every intermediate result and keep each until the expression finishes.
[[nodiscard]] lazy_expression_measurement
run_lazy_expression_eager(const lazy_expression_input& input) {
    const lazy_expression_size logical_length = input.source_logical_length;
    auto source_owner = bioseq::storage::make_resident_lazy_sequence(input.source);
    const lazy_expression_facade source = bioseq::storage::make_lazy_sequence_view(source_owner);
    lazy_expression_allocation_statistics stats{};
    const std::uint64_t reserved_baseline =
        bioseq::parallelism::memory_admission_controller::instance().reserved_bytes();
    lazy_expression_measurement output{};
    {
        const lazy_expression_node a_node =
            make_lazy_expression_slice(source, logical_length / 8u, 5u * logical_length / 8u);
        const lazy_expression_facade a_expression = lazy_expression_view(a_node);
        const lazy_expression_materialization a =
            materialize_lazy_expression_view(a_expression, false, stats);

        const lazy_expression_node b_node =
            make_lazy_expression_slice(source, logical_length / 2u, 7u * logical_length / 8u);
        const lazy_expression_facade b_expression = lazy_expression_view(b_node);
        const lazy_expression_materialization b =
            materialize_lazy_expression_view(b_expression, false, stats);

        const lazy_expression_node c_node = make_lazy_expression_concat(a.view(), b.view());
        const lazy_expression_facade c_expression = lazy_expression_view(c_node);
        const lazy_expression_materialization c =
            materialize_lazy_expression_view(c_expression, false, stats);

        const lazy_expression_node d_node =
            make_lazy_expression_slice(c.view(), logical_length / 4u, 3u * logical_length / 4u);
        const lazy_expression_facade d_expression = lazy_expression_view(d_node);
        const lazy_expression_materialization d =
            materialize_lazy_expression_view(d_expression, false, stats);

        const lazy_expression_node e_node =
            make_lazy_expression_slice(a.view(), logical_length / 8u, 3u * logical_length / 8u);
        const lazy_expression_facade e_expression = lazy_expression_view(e_node);
        const lazy_expression_materialization e =
            materialize_lazy_expression_view(e_expression, false, stats);

        const lazy_expression_node result_node = make_lazy_expression_concat(d.view(), e.view());
        const lazy_expression_facade result_expression = lazy_expression_view(result_node);
        const lazy_expression_materialization result =
            materialize_lazy_expression_view(result_expression, true, stats);
        output = finish_lazy_expression_run(result);
    }
    validate_lazy_expression_run_released(stats, reserved_baseline);
    output.allocations = stats;
    return output;
}

/// Keep, intermediate operations as views and copy only the final result.
[[nodiscard]] lazy_expression_measurement
run_lazy_expression_deferred(const lazy_expression_input& input) {
    const lazy_expression_size logical_length = input.source_logical_length;
    auto source_owner = bioseq::storage::make_resident_lazy_sequence(input.source);
    const lazy_expression_facade source = bioseq::storage::make_lazy_sequence_view(source_owner);
    lazy_expression_allocation_statistics stats{};
    const std::uint64_t reserved_baseline =
        bioseq::parallelism::memory_admission_controller::instance().reserved_bytes();
    lazy_expression_measurement output{};
    {
        const lazy_expression_node a_node =
            make_lazy_expression_slice(source, logical_length / 8u, 5u * logical_length / 8u);
        const lazy_expression_facade a = lazy_expression_view(a_node);
        const lazy_expression_node b_node =
            make_lazy_expression_slice(source, logical_length / 2u, 7u * logical_length / 8u);
        const lazy_expression_facade b = lazy_expression_view(b_node);
        const lazy_expression_node c_node = make_lazy_expression_concat(a, b);
        const lazy_expression_facade c = lazy_expression_view(c_node);
        const lazy_expression_node d_node =
            make_lazy_expression_slice(c, logical_length / 4u, 3u * logical_length / 4u);
        const lazy_expression_facade d = lazy_expression_view(d_node);
        const lazy_expression_node e_node =
            make_lazy_expression_slice(a, logical_length / 8u, 3u * logical_length / 8u);
        const lazy_expression_facade e = lazy_expression_view(e_node);
        const lazy_expression_node result_node = make_lazy_expression_concat(d, e);
        const lazy_expression_facade result_expression = lazy_expression_view(result_node);
        const lazy_expression_materialization result =
            materialize_lazy_expression_view(result_expression, true, stats);
        output = finish_lazy_expression_run(result);
    }
    validate_lazy_expression_run_released(stats, reserved_baseline);
    output.allocations = stats;
    return output;
}

/// Run the configured expression benchmark or reject an unknown algorithm.
[[nodiscard]] lazy_expression_measurement run_lazy_expression(std::string_view algorithm,
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

/// Return the packed byte count for a DNA4 sequence of this length.
[[nodiscard]] constexpr std::uint64_t
lazy_expression_payload_bytes(lazy_expression_size bp) noexcept {
    constexpr std::uint64_t letters_per_unit = 32u;
    const std::uint64_t units =
        bp / letters_per_unit + static_cast<std::uint64_t>(bp % letters_per_unit != 0u);
    return units * lazy_expression_storage_width;
}

/// Return the  total packed bytes expected from the chosen evaluation strategy.
[[nodiscard]] std::uint64_t
expected_lazy_expression_payload_bytes(bool eager, lazy_expression_size logical_length) noexcept {
    if (!eager) {
        return lazy_expression_payload_bytes(3u * logical_length / 4u);
    }
    return lazy_expression_payload_bytes(logical_length / 2u) +
           lazy_expression_payload_bytes(3u * logical_length / 8u) +
           lazy_expression_payload_bytes(7u * logical_length / 8u) +
           lazy_expression_payload_bytes(logical_length / 2u) +
           lazy_expression_payload_bytes(logical_length / 4u) +
           lazy_expression_payload_bytes(3u * logical_length / 4u);
}

/// Return the total memory reservation expected from the chosen strategy.
[[nodiscard]] std::uint64_t
expected_lazy_expression_admitted_bytes(bool eager, lazy_expression_size logical_length) {
    // Compute the memory reservation expected for one packed result.
    const auto admitted = [](lazy_expression_size bp) {
        return bioseq::storage::packed_region_admission_bytes<seq::dna4>(
            bp, lazy_expression_storage_width);
    };
    if (!eager) {
        return admitted(3u * logical_length / 4u);
    }
    return admitted(logical_length / 2u) + admitted(3u * logical_length / 8u) +
           admitted(7u * logical_length / 8u) + admitted(logical_length / 2u) +
           admitted(logical_length / 4u) + admitted(3u * logical_length / 4u);
}

/// Check the output against the expected length and hash, then verify allocation
/// totals and release of all reservations. Return the benchmark metrics.
[[nodiscard]] std::map<std::string, double>
evaluate_lazy_expression(std::string_view algorithm,
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
        output.output_logical_length == input.expected_output_logical_length &&
        output.logical_hash == input.expected_hash;
    const bool allocation_ok =
        allocations.payload_materializations == expected_materializations &&
        allocations.intermediate_payload_materializations == expected_intermediate &&
        allocations.boundary_materializations == 1u &&
        allocations.payload_bytes_materialized == expected_payload &&
        allocations.live_payload_bytes == 0u &&
        allocations.peak_live_payload_bytes == expected_payload &&
        allocations.admitted_bytes_total == expected_admitted &&
        allocations.live_admitted_bytes == 0u &&
        allocations.peak_live_admitted_bytes == expected_admitted;
    if (!correctness_ok || !allocation_ok) {
        throw std::runtime_error("lazy-expression correctness/allocation contract failed");
    }

    std::map<std::string, double> metrics;
    metrics.emplace("output_bp", static_cast<double>(output.output_logical_length));
    metrics.emplace("logical_hash_hi32",
                    static_cast<double>(static_cast<std::uint32_t>(output.logical_hash >> 32u)));
    metrics.emplace("logical_hash_lo32",
                    static_cast<double>(static_cast<std::uint32_t>(output.logical_hash)));
    metrics.emplace("payload_materializations",
                    static_cast<double>(allocations.payload_materializations));
    metrics.emplace("intermediate_payload_materializations",
                    static_cast<double>(allocations.intermediate_payload_materializations));
    metrics.emplace("boundary_materializations",
                    static_cast<double>(allocations.boundary_materializations));
    metrics.emplace("payload_bytes_materialized",
                    static_cast<double>(allocations.payload_bytes_materialized));
    metrics.emplace("peak_live_payload_bytes",
                    static_cast<double>(allocations.peak_live_payload_bytes));
    metrics.emplace("admitted_bytes_total", static_cast<double>(allocations.admitted_bytes_total));
    metrics.emplace("peak_live_admitted_bytes",
                    static_cast<double>(allocations.peak_live_admitted_bytes));
    metrics.emplace("correctness_contract_ok", correctness_ok ? 1.0 : 0.0);
    metrics.emplace("allocation_contract_ok", allocation_ok ? 1.0 : 0.0);
    return metrics;
}

/// No cleanup is needed here. The input releases its memory when destroyed.
void cleanup_lazy_expression(std::string_view,
                             std::string_view,
                             simdjson::dom::element,
                             const lazy_expression_input&) noexcept {}


/// Construct a known-valid fixture operation without maintaining a second opcode mapping.
[[nodiscard]] cigar_run cigar_benchmark_run(std::uint32_t length, char operation) {
    return cigar_run{length, cigar_symbol{}.assign_char(operation)};
}

/// Encode canonical fixture runs outside timing using the production counted-run codec.
[[nodiscard]] cigar_benchmark_value encode_cigar_benchmark_value(std::vector<cigar_run> runs) {
    const auto bytes = measure_cigar_payload<cigar_symbol>(static_cast<std::uint32_t>(runs.size()));
    if (!bytes) throw std::runtime_error("CIGAR benchmark payload measurement failed");
    cigar_benchmark_value value{.literal_runs = std::move(runs),
                                .payload = std::vector<std::byte>(*bytes)};
    const auto encoded = write_cigar_payload<cigar_symbol>(
        value.payload, value.literal_runs, cigar_space::compatibility);
    if (!encoded) throw std::runtime_error("CIGAR benchmark fixture encoding failed");
    value.logical_length = *encoded;
    return value;
}

/// Borrow a prevalidated encoded fixture; its bytes remain owned by the cached input.
[[nodiscard]] compact_sequence_range<cigar_symbol>
cigar_benchmark_view(const cigar_benchmark_value& value) {
    return compact_sequence_range<cigar_symbol>{value.payload, value.logical_length};
}

/// Reject unreasonable fixture dimensions before either estimation or allocation.
[[nodiscard]] std::size_t cigar_benchmark_dimension(
        simdjson::dom::element data, std::string_view key, std::int64_t fallback) {
    const auto configured = read_int_field(data, key, fallback);
    if (configured < 4 || configured > 256)
        throw std::invalid_argument("CIGAR benchmark rows/profile_intervals must be in [4,256]");
    return static_cast<std::size_t>(configured);
}

/// Build arithmetic oracles for counted processing and six independent ordering tiers.
[[nodiscard]] cigar_benchmark_fixture build_cigar_benchmark_fixture(
        std::string_view scenario, std::size_t rows, std::size_t intervals, std::uint64_t seed) {
    cigar_benchmark_fixture fixture;
    const auto run = cigar_benchmark_run;
    if (scenario == "literal" || scenario == "huge_N_2000" ||
        scenario == "huge_N_2000000000") {
        const auto count = scenario == "literal" ? 64u :
            scenario == "huge_N_2000" ? 2000u : 2000000000u;
        fixture.expected_count = count;
        fixture.values.push_back(encode_cigar_benchmark_value(
            {run(count, scenario == "literal" ? 'M' : 'N')}));
        return fixture;
    }
    const bool tiny = scenario == "tiny_256" || scenario == "tiny_16384";
    const bool filtering = scenario == "filter_256" || scenario == "filter_16384";
    const bool stripping = scenario == "strip_256" || scenario == "strip_16384";
    if (tiny || filtering || stripping) {
        const std::size_t count = scenario.ends_with("16384") ? 16384u : 256u;
        fixture.operation = filtering ? cigar_benchmark_operation::filter :
            stripping ? cigar_benchmark_operation::strip : cigar_benchmark_operation::count;
        std::vector<cigar_run> source;
        source.reserve(count + static_cast<std::size_t>(stripping));
        for (std::size_t index = 0u; index < count / 4u; ++index) {
            source.push_back(run(1u, stripping ? 'D' : 'M'));
            source.push_back(run(1u, 'I'));
            source.push_back(run(1u, stripping ? 'N' : '='));
            source.push_back(run(1u, 'I'));
            if (filtering || stripping) {
                fixture.expected_runs.push_back(run(1u, stripping ? 'D' : 'M'));
                fixture.expected_runs.push_back(run(1u, stripping ? 'N' : '='));
            }
        }
        if (stripping) {
            source.push_back(run(1u, 'M'));
            fixture.expected_runs.push_back(run(1u, 'M'));
        }
        fixture.expected_count = source.size();
        fixture.values.push_back(encode_cigar_benchmark_value(std::move(source)));
        return fixture;
    }
    if (scenario != "identical" && scenario != "totals" && scenario != "largest" &&
        scenario != "deep_profile" && scenario != "profile_equal" && scenario != "no_match")
        throw std::invalid_argument("unknown CIGAR benchmark scenario");
    fixture.operation = cigar_benchmark_operation::order;
    std::vector<std::size_t> ordinals(rows);
    std::iota(ordinals.begin(), ordinals.end(), std::size_t{0u});
    std::mt19937_64 random(seed);
    std::shuffle(ordinals.begin(), ordinals.end(), random);
    for (const auto ordinal : ordinals) {
        std::vector<cigar_run> source;
        const auto ordinal32 = static_cast<std::uint32_t>(ordinal);
        const auto rows32 = static_cast<std::uint32_t>(rows);
        if (scenario == "identical") source.push_back(run(64u, 'M'));
        else if (scenario == "totals") source.push_back(run(ordinal32 + 1u, 'M'));
        else if (scenario == "no_match") source = {run(ordinal32 + 1u, 'I'), run(1u, 'D')};
        else if (scenario == "largest") source = {
            run(ordinal32 + 1u, 'M'), run(1u, 'I'), run(2u * (rows32 + 2u) - ordinal32 - 1u, 'M')};
        else {
            for (std::size_t interval = 0u; interval < intervals; ++interval) {
                const auto length = scenario == "profile_equal" || interval < intervals - 2u
                    ? rows32 * 8u : interval == intervals - 2u
                        ? rows32 + ordinal32 + 1u : 3u * rows32 - ordinal32 - 1u;
                source.push_back(run(length, 'M'));
                if (interval + 1u < intervals) source.push_back(run(
                    scenario == "profile_equal" && interval == 0u ? ordinal32 + 1u : 1u, 'I'));
            }
        }
        fixture.values.push_back(encode_cigar_benchmark_value(std::move(source)));
    }
    fixture.expected_order.resize(rows);
    std::iota(fixture.expected_order.begin(), fixture.expected_order.end(), std::size_t{0u});
    if (scenario != "identical") {
        const bool reverse_ordinals = scenario == "largest" || scenario == "deep_profile";
        // These fixtures have unique arithmetic ordering. No production CIGAR comparator
        // or match-profile collector participates in constructing the oracle.
        std::sort(fixture.expected_order.begin(), fixture.expected_order.end(),
            [&ordinals, reverse_ordinals](std::size_t left, std::size_t right) {
                return reverse_ordinals ? ordinals[left] > ordinals[right] : ordinals[left] < ordinals[right];
            });
    }
    return fixture;
}

/// Estimate encoded fixtures, literal oracles, profiles, and existing context block classes.
[[nodiscard]] run_constraints estimate_cigar_benchmark_resources(
        std::string_view, std::string_view, simdjson::dom::element data) {
    const auto rows = cigar_benchmark_dimension(data, "rows", 64);
    const auto intervals = cigar_benchmark_dimension(data, "profile_intervals", 64);
    // All default scenarios together fit this conservative bound; positions are
    // never expanded, so the two-billion-position N fixture costs one run.
    const std::size_t fixture_bytes = 6u * 16385u * 32u + 6u * rows * intervals * 64u;
    return {.base_memory = fixture_bytes, .extra_memory = 16u * 1024u * 1024u + rows * intervals * 64u};
}

/// Cache only selected scenarios and initialize the production memory owner outside timing.
[[nodiscard]] cigar_benchmark_input build_cigar_benchmark_input(
        std::string_view, std::string_view, simdjson::dom::element data,
        const run_constraints& constraints) {
    const auto rows = cigar_benchmark_dimension(data, "rows", 64);
    const auto intervals = cigar_benchmark_dimension(data, "profile_intervals", 64);
    const auto seed = static_cast<std::uint64_t>(read_int_field(data, "seed", 377019));
    cigar_benchmark_input input;
    for (const auto& tuple : constraints.execution_tuples) {
        const std::string scenario{tuple.params.at("scenario").as_string()};
        if (!input.fixtures.contains(scenario))
            input.fixtures.emplace(scenario, build_cigar_benchmark_fixture(scenario, rows, intervals, seed));
    }
    input.workspace = std::make_unique<cigar_benchmark_workspace>();
    return input;
}

/// Count actual backend run cursors and selection callbacks, preserving the flat fast path.
class counted_cigar_benchmark_backend : public flat_cigar_backend<cigar_symbol> {
    using base_type = flat_cigar_backend<cigar_symbol>;
    cigar_benchmark_measurement& statistics_;
public:
    /// Wrap a base cursor without moving it or expanding its counted runs.
    class run_cursor_type {
        base_type::run_cursor_type cursor_;
        cigar_benchmark_measurement& statistics_;
    public:
        using run_type = cigar_run;
        /// Open an independent production cursor and bind its visit counter.
        run_cursor_type(const base_type& backend, sequence_view_type view,
                        storage::sequence_size_t from, storage::sequence_size_t to,
                        cigar_benchmark_measurement& statistics)
            : cursor_(backend.runs(view, from, to)), statistics_(statistics) {}
        /// Count each emitted source run exactly once, including partial traversal.
        [[nodiscard]] std::expected<std::optional<cigar_run>, cigar_error> next() {
            auto next = cursor_.next();
            if (next && *next) ++statistics_.backend_run_visits;
            return next;
        }
    };
    /// Use the existing admitted arenas; instrumentation adds no allocation policy.
    counted_cigar_benchmark_backend(context::context& memory, cigar_benchmark_measurement& statistics)
        : base_type(memory), statistics_(statistics) {}
    /// Instrument only real backend traversal, not inferred positional work.
    [[nodiscard]] run_cursor_type runs(sequence_view_type view, storage::sequence_size_t from,
                                       storage::sequence_size_t to) const {
        return run_cursor_type{*this, view, from, to, statistics_};
    }
    /// Preserve the production one-pass retained-run builder while counting its input visits.
    template<class Predicate>
    [[nodiscard]] std::expected<sequence_type, cigar_error> select_runs(
            sequence_view_type view, Predicate&& keep) {
        return base_type::select_runs(view,
            [&keep, &statistics = statistics_](cigar_run run, storage::sequence_size_t position) {
                ++statistics.backend_run_visits;
                return keep(run, position);
            });
    }
};
static_assert(cigar_sequence_backend<counted_cigar_benchmark_backend>);

/// Borrow the existing context only when the production scalar comparator requests scratch.
struct cigar_benchmark_scratch_provider {
    context::context& memory;
    cigar_benchmark_measurement& statistics;
    /// Record real lazy scratch acquisition without constructing another budget service.
    [[nodiscard]] std::expected<std::reference_wrapper<context::context>, cigar_error> acquire() {
        ++statistics.scratch_acquire_calls;
        return std::ref(memory);
    }
};

/// Execute core weighted operations or compare scalar ordering against prepared profiles.
[[nodiscard]] cigar_benchmark_measurement run_cigar_benchmark(
        std::string_view algorithm, const parameter_map& parameters, const cigar_benchmark_input& input) {
    const auto& fixture = input.fixtures.at(std::string{parameters.at("scenario").as_string()});
    auto& workspace = *input.workspace;
    auto& memory = workspace.admitted.memory;
    cigar_benchmark_measurement output;
    counted_cigar_benchmark_backend backend(memory, output);
    output.reserved_before_bytes = workspace.controller.reserved_bytes();
    const auto now = bioseq::instrumentation::clock::now_ns;
    const auto started = now();
    if (algorithm == "bioseq_cigar_literal_sum") {
        if (fixture.operation != cigar_benchmark_operation::count)
            throw std::invalid_argument("literal baseline only measures canonical run counting");
        for (const auto run : fixture.values.front().literal_runs) {
            output.observed_count += run.length;
            ++output.backend_run_visits;
        }
        output.operation_ns = now() - started;
    } else if (algorithm == "bioseq_cigar_core") {
        const auto view = cigar_benchmark_view(fixture.values.front());
        if (fixture.operation == cigar_benchmark_operation::count) {
            const auto counted = cigar_count(backend, view,
                cigar_space_mask<cigar_symbol>(cigar_space::compatibility));
            output.operation_ns = now() - started;
            if (!counted) throw std::runtime_error("CIGAR weighted count failed");
            output.observed_count = *counted;
        } else {
            auto selected = fixture.operation == cigar_benchmark_operation::filter
                ? cigar_filter(backend, view, cigar_space_mask<cigar_symbol>(cigar_space::pairwise_dense))
                : cigar_strip(backend, view, true, true);
            output.operation_ns = now() - started;
            if (!selected) throw std::runtime_error("CIGAR filter/strip failed");
            const auto selected_view = backend.borrow(*selected);
            output.encoded_output_bytes = selected_view.encoded_payload_bytes();
            output.reserved_at_result_bytes = workspace.controller.reserved_bytes();
            output.output_runs.assign(selected_view.begin(), selected_view.end());
        }
    } else {
        if (fixture.operation != cigar_benchmark_operation::order ||
            (algorithm != "bioseq_cigar_scalar_order" && algorithm != "bioseq_cigar_prepared_order"))
            throw std::invalid_argument("unknown CIGAR benchmark algorithm/workload combination");
        std::vector<compact_sequence_range<cigar_symbol>> views;
        views.reserve(fixture.values.size());
        for (const auto& value : fixture.values) views.push_back(cigar_benchmark_view(value));
        if (algorithm == "bioseq_cigar_scalar_order") {
            cigar_benchmark_scratch_provider provider{memory, output};
            const auto sort_started = now();
            output.order = storage::sort_batch_permutation(views, {},
                [&backend, &provider, &output](auto left, auto right) {
                    ++output.comparator_calls;
                    const auto compared = compare_cigar(backend, left, right, provider);
                    if (!compared) throw std::runtime_error("CIGAR scalar ordering failed");
                    return *compared;
                }, false);
            output.sort_ns = now() - sort_started;
        } else {
            const auto scope = memory.push<context::arena::scratch>();
            auto keys = memory.make_scratch_vector<cigar_order_key<cigar_symbol>>(views.size());
            std::size_t profile_count{};
            for (const auto view : views) {
                const auto summary = summarize_cigar_order(backend, view);
                if (!summary) throw std::runtime_error("CIGAR ordering summary failed");
                profile_count += summary->match_intervals;
                keys.push_back({view, *summary, {}});
            }
            auto profiles = memory.alloc_scratch_array<storage::sequence_size_t>(profile_count);
            std::size_t offset{};
            for (auto& key : keys) {
                auto profile = profiles.subspan(offset, key.summary.match_intervals);
                offset += profile.size();
                const auto collected = collect_cigar_match_profile(backend, key.canonical, profile);
                if (!collected) throw std::runtime_error("CIGAR prepared profile collection failed");
                if (!profile.empty()) ++output.prepared_profile_arrays;
                output.prepared_profile_lengths += profile.size();
                if (profile.size() > 1u) {
                    bioseq::simd::sorting::sort_unsigned_64_descending(profile.data(), profile.size());
                    ++output.prepared_profile_sorts;
                }
                key.descending_match_lengths = profile;
            }
            output.preparation_ns = now() - started;
            const auto sort_started = now();
            output.order = storage::sort_batch_permutation(
                std::span<const cigar_order_key<cigar_symbol>>{keys.data(), keys.size()}, {},
                [&output](const auto& left, const auto& right) {
                    ++output.comparator_calls;
                    return compare_cigar_order_keys(left, right);
                }, false);
            output.sort_ns = now() - sort_started;
            output.reserved_at_result_bytes = workspace.controller.reserved_bytes();
        }
        output.operation_ns = now() - started;
    }
    if (fixture.operation == cigar_benchmark_operation::count || algorithm == "bioseq_cigar_scalar_order")
        output.reserved_at_result_bytes = workspace.controller.reserved_bytes();
    output.retained_after_bytes = workspace.admitted.retention.retained_bytes();
    return output;
}

/// Check independent result oracles and export observed counters without fabricated peak/PG metrics.
[[nodiscard]] std::map<std::string, double> evaluate_cigar_benchmark(
        std::string_view algorithm, const parameter_map& parameters, const cigar_benchmark_input& input,
        const cigar_benchmark_measurement& output) {
    const auto& fixture = input.fixtures.at(std::string{parameters.at("scenario").as_string()});
    const bool correct = fixture.operation == cigar_benchmark_operation::order
        ? output.order == fixture.expected_order : fixture.operation == cigar_benchmark_operation::count
            ? output.observed_count == fixture.expected_count : output.output_runs == fixture.expected_runs;
    if (!correct) throw std::runtime_error("CIGAR benchmark independent correctness contract failed");
    std::uint64_t input_runs{}, input_positions{}, encoded_bytes{};
    for (const auto& value : fixture.values) {
        input_runs += value.literal_runs.size();
        input_positions += value.logical_length;
        encoded_bytes += value.payload.size();
    }
    const std::string_view scenario = parameters.at("scenario").as_string();
    const bool prepared = algorithm == "bioseq_cigar_prepared_order";
    if (fixture.operation != cigar_benchmark_operation::order || prepared) {
        const auto passes = prepared || fixture.operation == cigar_benchmark_operation::strip ? 2u : 1u;
        if (output.backend_run_visits != passes * input_runs)
            throw std::runtime_error("CIGAR benchmark weighted traversal count changed");
    }
    if (algorithm == "bioseq_cigar_scalar_order") {
        const bool profile_required = scenario == "deep_profile" || scenario == "profile_equal";
        if ((output.scratch_acquire_calls != 0u) != profile_required)
            throw std::runtime_error("CIGAR benchmark scalar ordering shortcut contract failed");
    }
    if (prepared) {
        const auto rows = fixture.values.size();
        const auto expected_arrays = scenario == "no_match" ? 0u : rows;
        const auto expected_sorts = scenario == "largest" || scenario == "deep_profile" ||
            scenario == "profile_equal" ? rows : 0u;
        if (output.prepared_profile_arrays != expected_arrays ||
            output.prepared_profile_sorts != expected_sorts || output.scratch_acquire_calls != 0u)
            throw std::runtime_error("CIGAR benchmark prepared-key amortization contract failed");
    }
    return {{"input_run_count", static_cast<double>(input_runs)},
            {"input_logical_positions", static_cast<double>(input_positions)},
            {"encoded_input_bytes", static_cast<double>(encoded_bytes)},
            {"output_run_count", static_cast<double>(output.output_runs.size())},
            {"encoded_output_bytes", static_cast<double>(output.encoded_output_bytes)},
            {"observed_count", static_cast<double>(output.observed_count)},
            {"backend_run_visits", static_cast<double>(output.backend_run_visits)},
            {"comparator_calls", static_cast<double>(output.comparator_calls)},
            {"scratch_acquire_calls", static_cast<double>(output.scratch_acquire_calls)},
            {"prepared_profile_arrays", static_cast<double>(output.prepared_profile_arrays)},
            {"prepared_profile_lengths", static_cast<double>(output.prepared_profile_lengths)},
            {"prepared_profile_sorts", static_cast<double>(output.prepared_profile_sorts)},
            {"operation_ns", static_cast<double>(output.operation_ns)},
            {"preparation_ns", static_cast<double>(output.preparation_ns)},
            {"sort_ns", static_cast<double>(output.sort_ns)},
            {"reserved_before_bytes", static_cast<double>(output.reserved_before_bytes)},
            {"reserved_at_result_bytes", static_cast<double>(output.reserved_at_result_bytes)},
            {"retained_after_bytes", static_cast<double>(output.retained_after_bytes)},
            {"parallel_sort", 0.0},
            {"correctness_contract_ok", 1.0}};
}

/// Cached fixture and admitted arena owners release themselves after the case group.
void cleanup_cigar_benchmark(std::string_view, std::string_view, simdjson::dom::element,
                             const cigar_benchmark_input&) noexcept {}

} // namespace bioseq::io::benchmark::detail
