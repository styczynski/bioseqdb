/**
 * @file lazy_expression.cppm
 * @brief Lazy slicing and concatenation benchmark workload.
 */
module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <libassert/assert.hpp>
#include <limits>
#include <map>
#include <memory>
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

} // namespace bioseq::io::benchmark::detail
