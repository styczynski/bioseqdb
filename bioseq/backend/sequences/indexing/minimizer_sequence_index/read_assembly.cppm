/**
 * @file minimizer_sequence_index/read_assembly.cppm
 * @brief Stable direct minimizer index operations and query assembly.
 */
module;

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <expected>
#include <iterator>
#include <libassert/assert.hpp>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

export module bioseq.indexing.minimizer_sequence:read_assembly;

import :admitted_runs;
import :bucket_codec;
import :replay_build;
import bioseq.common.checked_arithmetic;
import bioseq.common.hashing;
import bioseq.common.seqan_adapter;
import bioseq.gpu.vulkan.abi;
import bioseq.gpu.vulkan.runtime;
import bioseq.indexing.minimizer_hashmap;
import bioseq.indexing.minimizer_index;
import bioseq.indexing.storage;
import bioseq.indexing.traits;
import bioseq.indexing.type;
import bioseq.parallelism.memory_admission;
import bioseq.parallelism.mode;
import bioseq.parallelism.pipeline_ops;
import bioseq.parallelism.pool;
import bioseq.sequences;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.vulkan_minimizers;
import bioseq.storage.lazy_sequence;
import bioseq.storage.size;

export namespace bioseq::indexing {

using bioseq::minimizers::minimap_sketch_data;
using bioseq::minimizers::minimizer_sketch_backend;
using bioseq::minimizers::minimizer_sketch_backend_name;
using bioseq::minimizers::sketch_collector;
using bioseq::simd::collect_minimizers_view;
using bioseq::storage::lazy_sequence;
using bioseq::storage::sequence_size_t;

template <class p_storage>
struct direct_minimizer_bucket_index {
    static_assert(is_index_chunked_storage_for<p_storage, sequence_index_layout::fragment_chunks>);
    static_assert(is_external_sort_spill_storage<p_storage>);
    static constexpr sequence_index_layout index_type = sequence_index_layout::fragment_chunks;
    static constexpr std::string_view  index_name = k_minimizers_per_sequence_index_name;
    using storage_type = p_storage;

    class generation_spill_result {
    private:
        friend struct direct_minimizer_bucket_index<p_storage>;

        std::uint64_t run_count_ = 0;
        exact_simd_index_execution_stats exact_simd_stats_{};
        bool exact_simd_stats_available_ = false;
        minimizer_scheme_identity minimizer_scheme_{};
        std::uint64_t target_length_ = 0;
        std::uint64_t position_segment_span_bp_ = 0;
        minimizer_sequence_generation_execution_receipt execution_receipt_{};
        std::shared_ptr<const direct_minimizer_spill_authority<p_storage>> authority_{};

        generation_spill_result(
                std::uint64_t run_count,
                const exact_simd_index_execution_stats& exact_simd_stats,
                bool exact_simd_stats_available,
                minimizer_scheme_identity minimizer_scheme,
                std::uint64_t target_length,
                std::uint64_t position_segment_span_bp,
                const minimizer_sequence_generation_execution_receipt& execution_receipt,
                std::shared_ptr<const direct_minimizer_spill_authority<p_storage>> authority) noexcept
            : run_count_(run_count),
              exact_simd_stats_(exact_simd_stats),
              exact_simd_stats_available_(exact_simd_stats_available),
              minimizer_scheme_(minimizer_scheme),
              target_length_(target_length),
              position_segment_span_bp_(position_segment_span_bp),
              execution_receipt_(execution_receipt),
              authority_(std::move(authority)) {}

        void poison_() noexcept {
            run_count_ = 0;
            exact_simd_stats_ = {};
            exact_simd_stats_available_ = false;
            minimizer_scheme_ = {};
            target_length_ = 0;
            position_segment_span_bp_ = 0;
            execution_receipt_ = {};
            authority_.reset();
        }

        void steal_from_(generation_spill_result&& other) noexcept {
            run_count_ = other.run_count_;
            exact_simd_stats_ = other.exact_simd_stats_;
            exact_simd_stats_available_ = other.exact_simd_stats_available_;
            minimizer_scheme_ = other.minimizer_scheme_;
            target_length_ = other.target_length_;
            position_segment_span_bp_ = other.position_segment_span_bp_;
            execution_receipt_ = std::move(other.execution_receipt_);
            authority_ = std::move(other.authority_);
            other.poison_();
        }

    public:
        generation_spill_result() = delete;
        generation_spill_result(const generation_spill_result&) = delete;
        generation_spill_result& operator=(const generation_spill_result&) = delete;
        generation_spill_result(generation_spill_result&& other) noexcept {
            steal_from_(std::move(other));
        }
        generation_spill_result& operator=(generation_spill_result&& other) noexcept {
            if (this != &other) {
                poison_();
                steal_from_(std::move(other));
            }
            return *this;
        }

        [[nodiscard]] std::uint64_t run_count() const noexcept {
            return run_count_;
        }
        [[nodiscard]] const exact_simd_index_execution_stats&
        exact_simd_stats() const noexcept {
            return exact_simd_stats_;
        }
        [[nodiscard]] bool exact_simd_stats_available() const noexcept {
            return exact_simd_stats_available_;
        }
        [[nodiscard]] minimizer_scheme_identity
        minimizer_scheme() const noexcept {
            return minimizer_scheme_;
        }
        [[nodiscard]] std::uint64_t target_length() const noexcept {
            return target_length_;
        }
        [[nodiscard]] std::uint64_t
        position_segment_span_bp() const noexcept {
            return position_segment_span_bp_;
        }
        [[nodiscard]] const minimizer_sequence_generation_execution_receipt&
        execution_receipt() const noexcept {
            return execution_receipt_;
        }
    };

    template <sequence_alphabet p_alphabet>
    [[nodiscard]] static std::expected<
        segment_unit_range,
        minimizer_generation_encoding_error>
    make_generation_collection_range(
            bioseq::storage::lazy_sequence_view<p_alphabet> sequence,
            std::uint64_t core_lo,
            std::uint64_t core_hi,
            std::uint64_t halo_bp = 10u + 15u - 2u) noexcept {
        const std::uint64_t target_length = sequence.bp_size();
        if (core_lo > core_hi || core_hi > target_length) {
            return std::unexpected(minimizer_generation_encoding_error::bp_range);
        }
        const std::uint64_t collection_lo = core_lo >= halo_bp
            ? core_lo - halo_bp : 0u;
        const std::uint64_t collection_hi = core_hi
                > target_length - std::min(target_length, halo_bp)
            ? target_length : core_hi + halo_bp;
        if (collection_lo > (std::numeric_limits<sequence_size_t>::max)()
            || collection_hi > (std::numeric_limits<sequence_size_t>::max)()) {
            return std::unexpected(
                minimizer_generation_encoding_error::arithmetic_overflow);
        }
        return segment_unit_range{
            .cs_u = static_cast<sequence_size_t>(collection_lo),
            .ce_u = static_cast<sequence_size_t>(collection_hi),
            .core_lo_bp = static_cast<sequence_size_t>(core_lo),
            .core_hi_bp = static_cast<sequence_size_t>(core_hi),
        };
    }

    template <sequence_alphabet p_alphabet>
    [[nodiscard]] static std::expected<
        exact_simd_index_execution_stats,
        minimizer_generation_encoding_error>
    collect_generation_core_run(
            bioseq::storage::lazy_sequence_view<p_alphabet> sequence,
            std::uint8_t alphabet_code,
            minimizer_sketch_backend backend,
            const bioseq::minimizers::minimizer_kernels* kernels,
            segment_unit_range range,
            sketch_collector& collector,
            std::vector<detail::sorted_pair_t>& output,
            minimizer_sequence_generation_execution_receipt* execution_receipt = nullptr,
            bool allow_packed_bounded = false,
            direct_minimizer_bucket_build_stats* scalar_build_stats = nullptr) {
        const std::uint64_t target_length = sequence.bp_size();
        if ((backend == minimizer_sketch_backend::exact_simd
                && (kernels == nullptr
                    || kernels->two_stack_nthash == nullptr))
            || (backend == minimizer_sketch_backend::bounded_nthash_simd
                && (kernels == nullptr
                    || kernels->bounded_nthash == nullptr))) {
            return std::unexpected(
                minimizer_generation_encoding_error::storage_contract_mismatch);
        }
        if (range.core_lo_bp > range.core_hi_bp
            || range.core_hi_bp > target_length
            || range.cs_u > range.ce_u || range.ce_u > target_length) {
            return std::unexpected(minimizer_generation_encoding_error::bp_range);
        }
        if (range.core_lo_bp == range.core_hi_bp
            || range.cs_u == range.ce_u) {
            return exact_simd_index_execution_stats{};
        }
        const std::uint64_t emit_base = range.cs_u;
        constexpr std::uint64_t halo_bp = 10u + 15u - 2u;
        const std::uint64_t collection_hi = range.core_hi_bp
                > target_length - std::min(target_length, halo_bp)
            ? target_length : range.core_hi_bp + halo_bp;
        const std::uint64_t local_bp_size = collection_hi > emit_base
            ? collection_hi - emit_base : 0u;
        constexpr std::uint64_t minimum_work_bases = 10u + 15u - 1u;
        const auto add_route = [execution_receipt](
                std::uint64_t minimizer_sequence_generation_execution_receipt::* member,
                std::uint64_t increment = 1u) noexcept {
            if (execution_receipt != nullptr) {
                minimizer_sequence_generation_execution_receipt::add(
                    execution_receipt->*member,
                    increment,
                    execution_receipt->saturated);
            }
        };
        if (local_bp_size < minimum_work_bases) {
            add_route(&minimizer_sequence_generation_execution_receipt::short_core_runs);
            return exact_simd_index_execution_stats{};
        }
        if (backend == minimizer_sketch_backend::vulkan_segment) {
            add_route(&minimizer_sequence_generation_execution_receipt::vulkan_core_runs);
            bioseq::gpu::vulkan::vulkan_segment_pair_execution_stats core_stats{};
            auto pairs = bioseq::gpu::vulkan::
                collect_sorted_minimizer_pairs_vulkan_segment_or_throw(
                    sequence,
                    range.cs_u,
                    range.ce_u,
                    static_cast<sequence_size_t>(emit_base),
                    range.core_lo_bp,
                    range.core_hi_bp,
                    10u,
                    15u,
                    execution_receipt != nullptr ? &core_stats : nullptr);
            if (execution_receipt != nullptr) {
                execution_receipt->vulkan_stats.merge(core_stats);
                execution_receipt->saturated = execution_receipt->saturated
                    || execution_receipt->vulkan_stats.saturated;
            }
            output.insert(
                output.end(),
                std::make_move_iterator(pairs.pairs.begin()),
                std::make_move_iterator(pairs.pairs.end()));
            return exact_simd_index_execution_stats{};
        }
        if (backend != minimizer_sketch_backend::scalar
            && backend != minimizer_sketch_backend::exact_simd
            && backend != minimizer_sketch_backend::bounded_nthash_simd) {
            return std::unexpected(
                minimizer_generation_encoding_error::storage_contract_mismatch);
        }

        collector.clear();
        exact_simd_index_execution_stats exact_simd_stats{};
        if (backend == minimizer_sketch_backend::exact_simd) {
            add_route(&minimizer_sequence_generation_execution_receipt::exact_simd_core_runs);
            exact_simd_stats = collect_exact_simd_index_range_or_throw(
                collector,
                sequence,
                alphabet_code,
                range.cs_u,
                range.ce_u,
                10u,
                15u,
                0u,
                kernels,
                "minimizers_per_sequence build");
            if (execution_receipt != nullptr) {
                execution_receipt->exact_simd_stats.merge(
                    exact_simd_stats);
                execution_receipt->exact_simd_stats_available = true;
                execution_receipt->saturated = execution_receipt->saturated
                    || execution_receipt->exact_simd_stats.saturated;
            }
        } else if (backend == minimizer_sketch_backend::bounded_nthash_simd) {
            if (!allow_packed_bounded) {
                return std::unexpected(
                    minimizer_generation_encoding_error::storage_contract_mismatch);
            }
            add_route(&minimizer_sequence_generation_execution_receipt::bounded_nthash_core_runs);
            std::uint64_t materialized_window_bp = 0u;
            const auto bounded_result =
                collect_packed_bounded_nthash_index_lazy(
                    sequence,
                    alphabet_code,
                    range.cs_u,
                    range.ce_u,
                    local_bp_size,
                    10u,
                    15u,
                    kernels,
                    collector,
                    &materialized_window_bp);
            if (execution_receipt != nullptr) {
                execution_receipt->peak_bounded_materialized_window_bp =
                    std::max(
                        execution_receipt->peak_bounded_materialized_window_bp,
                        materialized_window_bp);
                execution_receipt->packed_bounded_status = bounded_result.status;
                execution_receipt->packed_bounded_stats.merge(
                    bounded_result.stats);
                execution_receipt->packed_bounded_stats_available = true;
                execution_receipt->saturated = execution_receipt->saturated
                    || execution_receipt->packed_bounded_stats.saturated;
            }
            if (bounded_result.status != packed_bounded_nthash_index_status::ok) {
                throw packed_bounded_nthash_index_runtime_error{
                    bounded_result.status,
                    "minimizers_per_sequence build"};
            }
        } else {
            add_route(&minimizer_sequence_generation_execution_receipt::scalar_core_runs);
            bioseq::simd::collect_minimizers_segment_scalar<
                minimap_sketch_data>(
                    collector, sequence, range.cs_u, range.ce_u, 10u, 15u);
            if (scalar_build_stats != nullptr) {
                scalar_build_stats->record_scalar_collection(
                    range.ce_u - range.cs_u,
                    range.ce_u - range.cs_u,
                    1u,
                    1u);
            }
        }
        for (const minimap_sketch_data& minimizer
             : collector.extract_sketches_data()) {
            const std::uint64_t position = emit_base
                + static_cast<std::uint64_t>(minimizer.last_pos());
            if (position < range.core_lo_bp
                || position >= range.core_hi_bp) {
                continue;
            }
            output.emplace_back(
                minimizer.kmer(),
                (position << 1u)
                    | static_cast<std::uint64_t>(minimizer.strand()));
        }
        return exact_simd_stats;
    }

    /// Prepare external runs serially within the bounded production budget. Each source core is
    /// collected and spilled before the next is allocated. The returned direct minimizer-bucket
    /// producer reopens these same external runs for both encoder passes.
    template <sequence_alphabet p_alphabet>
    [[nodiscard]] static std::expected<
        generation_spill_result,
        minimizer_generation_encoding_error>
    spill_generation_replay_runs_serial(
            p_storage& storage,
            bioseq::storage::lazy_sequence_view<p_alphabet> sequence,
            minimizer_sketch_backend backend,
            const bioseq::minimizers::minimizer_kernels* kernels,
            std::uint64_t core_span_bp = 0,
            std::uint64_t position_segment_span_bp =
                k_minimizer_preferred_segment_span_bp,
            minimizer_sequence_generation_execution_receipt* execution_receipt_out = nullptr,
            std::uint8_t alphabet_code =
                bioseq::minimizers::k_unbound_packed_alphabet_code) {
        static_assert(direct_minimizer_qualified_spill_storage<p_storage>);
        constexpr std::uint64_t default_core_span = 32'768u;
        constexpr std::uint64_t lpu = 1u;
        const std::uint64_t target_length = sequence.bp_size();
        struct receipt_output_guard {
            minimizer_sequence_generation_execution_receipt* output = nullptr;
            minimizer_sequence_generation_execution_receipt receipt{};
            ~receipt_output_guard() {
                if (output != nullptr) {
                    *output = receipt;
                }
            }
        } receipt_guard;
        receipt_guard.output = execution_receipt_out;
        minimizer_sequence_generation_execution_receipt& receipt = receipt_guard.receipt;
        receipt.selected_backend = backend;
        receipt.minimizer_scheme =
            minimizer_scheme_identity_for_backend(backend);
        receipt.target_length = target_length;
        receipt.window_size = 10;
        receipt.kmer_size = 15;
        receipt.configured_core_span_bp =
            core_span_bp == 0 ? default_core_span : core_span_bp;
        if ((backend == minimizer_sketch_backend::exact_simd
                && (kernels == nullptr
                    || kernels->two_stack_nthash == nullptr))
            || (backend == minimizer_sketch_backend::bounded_nthash_simd
                && (kernels == nullptr
                    || kernels->bounded_nthash == nullptr))) {
            return std::unexpected(
                minimizer_generation_encoding_error::storage_contract_mismatch);
        }
        if (core_span_bp == 0) {
            core_span_bp = default_core_span;
        }
        if (core_span_bp == 0 || core_span_bp > default_core_span
            || position_segment_span_bp == 0) {
            return std::unexpected(minimizer_generation_encoding_error::segment_span);
        }
        const minimizer_scheme_identity minimizer_scheme =
            minimizer_scheme_identity_for_backend(backend);
        if (!is_supported_minimizer_scheme_identity(
                minimizer_scheme.id, minimizer_scheme.revision)) {
            return std::unexpected(
                minimizer_generation_encoding_error::storage_contract_mismatch);
        }
        direct_minimizer_spill_storage_binding storage_binding =
            storage.begin_segment_spill_generation();
        if (!storage_binding.valid()) {
            return std::unexpected(minimizer_generation_encoding_error::admission);
        }
        receipt.backend_bound = true;
        std::uint64_t segment_lo = 0;
        std::uint64_t segment_ordinal = 0;
        {
        struct replay_buffers {
            // Reverse destruction drops both vector owners before admission.
            bioseq::parallelism::memory_permit permit;
            std::vector<detail::sorted_pair_t> run;
            sketch_collector collector;
        } buffers;
        const auto ensure_replay_capacity = [&buffers](std::uint64_t expected_run,
                                                       std::uint64_t expected_sketches) -> bool {
            constexpr std::uint64_t max = (std::numeric_limits<std::uint64_t>::max)();
            if (expected_run
                    > static_cast<std::uint64_t>(
                        (std::numeric_limits<std::size_t>::max)())
                || expected_sketches
                    > static_cast<std::uint64_t>(
                        (std::numeric_limits<std::size_t>::max)())) {
                return false;
            }
            const std::uint64_t run_bytes =
                bioseq::common::saturating_mul(
                    static_cast<std::uint64_t>(buffers.run.capacity()),
                    static_cast<std::uint64_t>(sizeof(detail::sorted_pair_t)));
            const std::uint64_t sketch_bytes =
                bioseq::common::saturating_mul(
                    static_cast<std::uint64_t>(
                        buffers.collector.extract_sketches_data().capacity()),
                    static_cast<std::uint64_t>(sizeof(minimap_sketch_data)));
            const std::uint64_t retained_bytes =
                bioseq::common::saturating_add(run_bytes, sketch_bytes);
            const std::uint64_t run_headroom =
                buffers.run.capacity() < expected_run
                    ? bioseq::common::saturating_mul(
                        bioseq::common::saturating_mul(
                            expected_run,
                            static_cast<std::uint64_t>(
                                sizeof(detail::sorted_pair_t))),
                        bioseq::parallelism::k_vector_preallocation_headroom_factor)
                    : 0u;
            const std::uint64_t sketch_headroom =
                buffers.collector.extract_sketches_data().capacity()
                        < expected_sketches
                    ? bioseq::common::saturating_mul(
                        bioseq::common::saturating_mul(
                            expected_sketches,
                            static_cast<std::uint64_t>(
                                sizeof(minimap_sketch_data))),
                        bioseq::parallelism::k_vector_preallocation_headroom_factor)
                    : 0u;
            const std::uint64_t allocation_peak =
                bioseq::common::saturating_add(
                    retained_bytes,
                    bioseq::common::saturating_add(
                        run_headroom, sketch_headroom));
            if (run_bytes == max || sketch_bytes == max || retained_bytes == max ||
                run_headroom == max || sketch_headroom == max || allocation_peak == max) {
                return false;
            }
            if (buffers.permit) {
                if (buffers.permit.bytes() < retained_bytes
                    || !buffers.permit.grow_to(allocation_peak)) {
                    return false;
                }
            } else {
                if (retained_bytes != 0u) {
                    return false;
                }
                auto permit =
                    bioseq::parallelism::memory_admission_controller::instance()
                        .try_reserve(allocation_peak);
                if (!permit) {
                    return false;
                }
                buffers.permit = std::move(*permit);
            }
            if (buffers.run.capacity() < expected_run) {
                buffers.run.reserve(static_cast<std::size_t>(expected_run));
            }
            if (buffers.collector.extract_sketches_data().capacity()
                    < expected_sketches) {
                buffers.collector.reserve(
                    static_cast<std::size_t>(expected_sketches));
            }
            const std::uint64_t exact_run =
                bioseq::common::saturating_mul(
                    static_cast<std::uint64_t>(buffers.run.capacity()),
                    static_cast<std::uint64_t>(sizeof(detail::sorted_pair_t)));
            const std::uint64_t exact_sketch =
                bioseq::common::saturating_mul(
                    static_cast<std::uint64_t>(
                        buffers.collector.extract_sketches_data().capacity()),
                    static_cast<std::uint64_t>(sizeof(minimap_sketch_data)));
            const std::uint64_t exact = bioseq::common::saturating_add(
                exact_run, exact_sketch);
            if (exact_run == max || exact_sketch == max || exact == max ||
                exact > buffers.permit.bytes()) {
                return false;
            }
            buffers.permit.shrink_to(exact);
            return true;
        };

        while (segment_lo < target_length) {
            if (!sql_integer_can_represent(segment_ordinal)) {
                return std::unexpected(
                    minimizer_generation_encoding_error::segment_count);
            }
            const std::uint64_t segment_hi = segment_lo + std::min(
                position_segment_span_bp, target_length - segment_lo);
            for (std::uint64_t core_lo = segment_lo;
                 core_lo < segment_hi;) {
                const std::uint64_t core_hi = core_lo
                    + std::min(core_span_bp, segment_hi - core_lo);
                auto range = make_generation_collection_range(
                    sequence, core_lo, core_hi);
                if (!range) {
                    return std::unexpected(range.error());
                }
                receipt.peak_core_span_bp = std::max(
                    receipt.peak_core_span_bp, core_hi - core_lo);
                buffers.run.clear();
                const std::uint64_t collection_units =
                    static_cast<std::uint64_t>(range->ce_u - range->cs_u);
                const std::uint64_t maximum_new_sketches =
                    bioseq::common::saturating_mul(collection_units, lpu);
                // The checked bounded ntHash kernel's exact-tie matrix can emit the
                // selected minimum plus at most one window span of tied records
                // candidate. Reserve that proven raw-output bound before the
                // kernel runs; canonical sort/unique below reduces it before
                // persistence. Other CPU collectors need at most one retained
                // vector slot per input bp on this bounded core route.
                constexpr std::uint64_t maximum_bounded_records_per_candidate =
                    10u + 1u;
                const std::uint64_t expected_sketches =
                    backend == minimizer_sketch_backend::vulkan_segment
                        ? 0u
                        : (backend == minimizer_sketch_backend::bounded_nthash_simd
                            ? bioseq::common::saturating_mul(
                                maximum_new_sketches,
                                maximum_bounded_records_per_candidate)
                            : maximum_new_sketches);
                const std::uint64_t expected_run =
                    backend == minimizer_sketch_backend::vulkan_segment
                        ? core_hi - core_lo
                        : expected_sketches;
                if (!ensure_replay_capacity(expected_run, expected_sketches)) {
                    return std::unexpected(
                        minimizer_generation_encoding_error::admission);
                }
                auto collected = collect_generation_core_run(
                    sequence,
                    alphabet_code,
                    backend,
                    kernels,
                    *range,
                    buffers.collector,
                    buffers.run,
                    &receipt,
                    true);
                if (!collected) {
                    return std::unexpected(collected.error());
                }
                const std::uint64_t retained_after_collection =
                    bioseq::common::saturating_add(
                        bioseq::common::saturating_mul(
                            static_cast<std::uint64_t>(buffers.run.capacity()),
                            static_cast<std::uint64_t>(
                                sizeof(detail::sorted_pair_t))),
                        bioseq::common::saturating_mul(
                            static_cast<std::uint64_t>(buffers.collector
                                .extract_sketches_data().capacity()),
                            static_cast<std::uint64_t>(
                                sizeof(minimap_sketch_data))));
                if (retained_after_collection
                        == (std::numeric_limits<std::uint64_t>::max)()
                    || retained_after_collection > buffers.permit.bytes()) {
                    return std::unexpected(
                        minimizer_generation_encoding_error::admission);
                }
                buffers.permit.shrink_to(retained_after_collection);
                detail::sort_pair_run(buffers.run);
                // A source occurrence selected by adjacent tied windows is one
                // persisted event. Canonicalize before spill so qualification,
                // authority receipt, and the encoder all count the same
                // `(hash, encoded_position)` stream. The external merge still
                // deduplicates across arbitrary runs as a defensive invariant.
                buffers.run.erase(
                    std::ranges::unique(buffers.run).begin(),
                    buffers.run.end());
                receipt.peak_core_events = std::max<std::uint64_t>(
                    receipt.peak_core_events, buffers.run.size());
                if (!buffers.run.empty()) {
                    const direct_minimizer_spill_storage_binding before_spill =
                        storage_binding;
                    storage.spill_sorted_segment_run(
                        static_cast<std::uint32_t>(segment_ordinal),
                        std::span<const detail::sorted_pair_t>{buffers.run});
                    storage_binding = storage.segment_spill_storage_binding();
                    if (!storage_binding.valid()
                        || storage_binding.generation_token
                            != before_spill.generation_token
                        || before_spill.mutation_revision
                            == (std::numeric_limits<std::uint64_t>::max)()
                        || storage_binding.mutation_revision
                            != before_spill.mutation_revision + 1u) {
                        return std::unexpected(
                            minimizer_generation_encoding_error::replay_mismatch);
                    }
                    minimizer_sequence_generation_execution_receipt::add(
                        receipt.spill_run_count, 1u, receipt.saturated);
                    minimizer_sequence_generation_execution_receipt::add(
                        receipt.spilled_event_count,
                        buffers.run.size(),
                        receipt.saturated);
                    if (receipt.saturated) {
                        return std::unexpected(
                            minimizer_generation_encoding_error::arithmetic_overflow);
                    }
                }
                core_lo = core_hi;
            }
            segment_lo = segment_hi;
            ++segment_ordinal;
        }
        } // replay vectors are destroyed before their shared permit.

        // Qualify the exact order-specific stream now resident in this storage
        // incarnation. Authorized half-reroutes issue a successor mutation
        // revision and adopt a new full SHA-256 only after hashing every event.
        struct spill_qualification {
            std::uint64_t event_count = 0;
            bool saturated = false;
        } qualification;
        auto qualification_producer =
            direct_minimizer_unbound_external_spill_replay_producer<p_storage>{
                storage,
                target_length,
                position_segment_span_bp,
                minimizer_scheme};
        const auto qualified = qualification_producer.scan_sorted_events(
            position_segment_span_bp,
            canonical_minimizer_event_receiver{
                .context = &qualification,
                .emit_fn = +[](void* context,
                               const canonical_minimizer_event& event)
                    -> std::expected<void, minimizer_generation_encoding_error> {
                    auto& state = *static_cast<spill_qualification*>(context);
                    minimizer_sequence_generation_execution_receipt::add(
                        state.event_count, 1u, state.saturated);
                    if (state.saturated) {
                        return std::unexpected(
                            minimizer_generation_encoding_error::arithmetic_overflow);
                    }
                    (void)event;
                    return {};
                },
            });
        if (!qualified) {
            return std::unexpected(qualified.error());
        }
        if (qualification.event_count != receipt.spilled_event_count) {
            return std::unexpected(
                minimizer_generation_encoding_error::duplicate_event);
        }
        const minimizer_sequence_generation_execution_receipt qualification_receipt =
            qualification_producer.execution_receipt();
        receipt.spill_content_digest =
            qualification_receipt.replay_first_completed_content_digest;
        receipt.spill_qualification_scan_count =
            qualification_producer.scan_count();
        receipt.spill_qualification_merge_count =
            qualification_producer.merge_pass_count();
        const std::uint64_t operation_epoch =
            minimizer_sequence_generation_execution_receipt::next_spill_operation_epoch();
        const std::uint64_t authority_seal =
            minimizer_sequence_generation_execution_receipt::next_spill_authority_seal();
        if (operation_epoch == 0 || authority_seal == 0) {
            return std::unexpected(
                minimizer_generation_encoding_error::arithmetic_overflow);
        }
        receipt.spill_operation_epoch = operation_epoch;
        receipt.spill_storage_binding = storage_binding;
        auto authority = std::shared_ptr<const direct_minimizer_spill_authority<p_storage>>{
            new direct_minimizer_spill_authority<p_storage>{
                storage,
                target_length,
                position_segment_span_bp,
                receipt.spill_run_count,
                receipt.spilled_event_count,
                receipt.spill_content_digest,
                storage_binding,
                operation_epoch,
                authority_seal,
                minimizer_scheme}};
        receipt.bind_spill_authority(
            authority_seal,
            receipt.spilled_event_count,
            receipt.spill_content_digest,
            storage_binding,
            operation_epoch);
        receipt.spill_complete = true;
        receipt.capture_route_snapshot_();
        if (!receipt.is_spill_valid()) {
            return std::unexpected(
                minimizer_generation_encoding_error::storage_contract_mismatch);
        }
        return generation_spill_result{
            receipt.spill_run_count,
            receipt.exact_simd_stats,
            receipt.exact_simd_stats_available,
            minimizer_scheme,
            target_length,
            position_segment_span_bp,
            receipt,
            std::move(authority)};
    }

    [[nodiscard]] static auto make_generation_replay_producer(
            p_storage& storage,
            std::uint64_t target_length,
            generation_spill_result&& spill,
            std::uint64_t position_segment_span_bp =
                k_minimizer_preferred_segment_span_bp) {
        static_assert(direct_minimizer_qualified_spill_storage<p_storage>);
        minimizer_sequence_generation_execution_receipt receipt = spill.execution_receipt_;
        std::shared_ptr<const direct_minimizer_spill_authority<p_storage>> authority =
            std::move(spill.authority_);
        const bool spill_identity_matches =
            authority != nullptr
            && authority->storage_ == &storage
            && authority->target_length_ == target_length
            && authority->initial_span_bp_ == position_segment_span_bp
            && authority->run_count_ == spill.run_count_
            && authority->event_count_ == receipt.spilled_event_count
            && authority->content_digest_ == receipt.spill_content_digest
            && authority->storage_binding_ == receipt.spill_storage_binding
            && storage.segment_spill_storage_binding()
                == authority->storage_binding_
            && authority->operation_epoch_ == receipt.spill_operation_epoch
            && authority->authority_seal_ != 0
            && authority->minimizer_scheme_ == spill.minimizer_scheme_
            && spill.target_length_ == target_length
            && spill.position_segment_span_bp_ == position_segment_span_bp
            && receipt.authority_matches(
                authority->authority_seal_,
                authority->event_count_,
                authority->content_digest_,
                authority->storage_binding_,
                authority->operation_epoch_);
        const bool spill_identity_is_consistent = spill_identity_matches
            && spill.minimizer_scheme_ == receipt.minimizer_scheme
            && spill.run_count_ == receipt.spill_run_count
            && spill.exact_simd_stats_available_
                == receipt.exact_simd_stats_available;
        if (!spill_identity_is_consistent) {
            receipt.backend_bound = false;
            authority.reset();
        }
        const minimizer_scheme_identity producer_scheme =
            spill_identity_is_consistent ? spill.minimizer_scheme_
                                      : minimizer_scheme_identity{};
        spill.poison_();
        return direct_minimizer_external_spill_replay_producer<p_storage>{
            storage, target_length, position_segment_span_bp,
            producer_scheme,
            receipt,
            std::move(authority),
            receipt.spill_storage_binding};
    }

    /// Encode grouped-SoA rows from a sealed direct minimizer-bucket spill. The layout
    /// consumes the sealed direct minimizer-bucket spill without conversion.
    template <minimizer_generation_row_sink p_sink>
    [[nodiscard]] static std::expected<
        encoded_grouped_soa_generation,
        minimizer_generation_encoding_error>
    encode_persisted_generation(
            p_storage& storage,
            p_sink& sink,
            generation_spill_result&& spill,
            const grouped_soa_generation_options& options,
            const grouped_soa_generation_limits& limits = {},
            minimizer_sequence_generation_execution_receipt* execution_receipt_out = nullptr) {
        if (execution_receipt_out != nullptr) {
            *execution_receipt_out = {};
        }
        if (options.layout
                != minimizer_generation_layout::grouped_soa_segment_local
            || options.window_size != 10 || options.kmer_size != 15
            || spill.position_segment_span_bp_
                != k_minimizer_preferred_segment_span_bp
            || spill.authority_ == nullptr
            || spill.authority_->storage_ != &storage
            || storage.segment_spill_storage_binding()
                != spill.authority_->storage_binding_
            || !spill.execution_receipt_.is_spill_valid()) {
            return std::unexpected(
                minimizer_generation_encoding_error::storage_contract_mismatch);
        }
        auto producer = make_generation_replay_producer(
            storage,
            options.target_length,
            std::move(spill),
            k_minimizer_preferred_segment_span_bp);
        auto encoded = encode_minimizer_generation(
            producer, sink, options, limits);
        minimizer_sequence_generation_execution_receipt receipt =
            producer.execution_receipt();
        if (encoded) {
            receipt.manifest_position_count =
                encoded->manifest.position_count;
            receipt.manifest_position_segment_span_bp =
                encoded->manifest.position_segment_span_bp;
            receipt.manifest_bound =
                encoded->manifest.layout == options.layout
                && encoded->manifest.layout
                    == receipt.generation_layout
                && encoded->manifest.target_length == receipt.target_length
                && encoded->manifest.position_count
                    == receipt.spilled_event_count
                && encoded->manifest.kmer_size == receipt.kmer_size
                && encoded->manifest.window_size == receipt.window_size
                && encoded->manifest.minimizer_scheme_id
                    == receipt.minimizer_scheme.id
                && encoded->manifest.minimizer_scheme_revision
                    == receipt.minimizer_scheme.revision
                && encoded->manifest.producer_backend_id
                    == minimizer_producer_backend_identity_for_backend(
                        receipt.selected_backend).id
                && encoded->manifest.producer_identity_revision
                    == k_minimizer_producer_identity_revision;
            if (receipt.manifest_bound) {
                receipt.bind_manifest_authority();
                receipt.bind_publication_authority(*encoded);
                receipt.capture_replay_snapshot_();
            }
            if (!receipt.is_internally_consistent()) {
                encoded = std::unexpected(
                    minimizer_generation_encoding_error::storage_contract_mismatch);
            }
        }
        if (execution_receipt_out != nullptr) {
            *execution_receipt_out = receipt;
        }
        return encoded;
    }

    template <sequence_alphabet p_alphabet>
    static void build(
        p_storage& storage,
        bioseq::storage::lazy_sequence_view<p_alphabet> seq,
        minimizer_sketch_backend backend,
        const bioseq::minimizers::minimizer_kernels* kernels,
        direct_minimizer_bucket_build_stats* scalar_build_stats_out = nullptr,
        std::uint8_t alphabet_code =
            bioseq::minimizers::k_unbound_packed_alphabet_code) {
        constexpr std::size_t w = 10, k = 15;
        constexpr std::size_t lpu = 1u;
        constexpr std::size_t frag_bp    = 32'768u;
        constexpr std::size_t window_span = w + k - 2u;
        static_assert(frag_bp < (std::size_t{1} << 28), "fragment core positions fit 28-bit field");
        using detail::k_encoded_header_bytes; using detail::k_encoded_bucket_bytes; using detail::encoded_bucket_record;
        constexpr std::size_t k_bucket_target_bytes = std::size_t{8} << 20;       // 8 MiB

        if (backend == minimizer_sketch_backend::exact_simd
            && (kernels == nullptr || kernels->two_stack_nthash == nullptr)) {
            throw exact_simd_index_runtime_error{
                exact_simd_index_status::invalid_state,
                "minimizers_per_sequence build"};
        }
        if (backend == minimizer_sketch_backend::bounded_nthash_simd
            && (kernels == nullptr || kernels->bounded_nthash == nullptr)) {
            throw packed_bounded_nthash_index_runtime_error{
                packed_bounded_nthash_index_status::invalid_state,
                "minimizers_per_sequence build"};
        }

        if (scalar_build_stats_out != nullptr) {
            *scalar_build_stats_out = {};
        }
        const bool capture_scalar_build_stats =
            scalar_build_stats_out != nullptr;
        direct_minimizer_bucket_build_stats scalar_build_stats{};
        if (capture_scalar_build_stats) {
            scalar_build_stats.bind_backend(
                backend,
                1u,
                1u);
        }

        const sequence_size_t total_bp = seq.bp_size();
        const std::size_t nfrag = static_cast<std::size_t>((total_bp + frag_bp - 1) / frag_bp);

        // ---- Pass 0: collect globalised (kmer, encoded) pairs, spill sorted runs.
        const std::size_t run_buffer_bytes = storage.run_buffer_bytes();
        using pair_t = detail::sorted_pair_t;
        using detail::admitted_pair_run;
        const auto make_core_range = [&seq](sequence_size_t core_lo, sequence_size_t core_hi) {
            auto range = make_generation_collection_range(
                seq, core_lo, core_hi, window_span);
            if (!range) {
                index_corruption_panic(
                    "minimizers_per_sequence build: invalid collection/core range");
            }
            return *range;
        };
        struct worker_range_result {
            detail::admitted_vector_run<admitted_pair_run> runs;
            detail::admitted_vector_run<segment_unit_range>
                deferred_for_backend_spill;
            direct_minimizer_bucket_build_stats scalar_build_stats{};
            std::exception_ptr error;
            segment_unit_range error_range{};
        };

        auto release_untransferred_results_noexcept =
            [](std::vector<worker_range_result>& results) noexcept {
                for (worker_range_result& result : results) {
                    for (admitted_pair_run& run : result.runs.pairs) {
                        run.release_capacity_before_permit();
                    }
                    result.runs.release_capacity_before_permit();
                    result.deferred_for_backend_spill
                        .release_capacity_before_permit();
                }
            };

        // under vulkan_segment the fused GPU sketch emits the
        // window's minimizers on-device; the globalised (kmer, encoded_pos)
        // pairs are sorted on the HOST, so there is no CPU minimizer COLLECT on
        // this path. The scalar branch is unchanged.
        auto collect_core_range_pairs = [&seq, backend, kernels, alphabet_code](
                                            sequence_size_t core_lo,
                                            sequence_size_t core_hi,
                                            sketch_collector& collector,
                                            std::vector<pair_t>& out,
                                            direct_minimizer_bucket_build_stats* scalar_stats) {
            if (core_hi <= core_lo) {
                return;
            }
            auto range = make_generation_collection_range(
                seq, core_lo, core_hi, window_span);
            if (!range || !collect_generation_core_run(
                    seq,
                    alphabet_code,
                    backend,
                    kernels,
                    *range,
                    collector,
                    out,
                    nullptr,
                    true,
                    scalar_stats)) {
                index_corruption_panic(
                    "minimizers_per_sequence build: collection/core geometry failed");
            }
        };

        auto collect_sort_and_spill_backend_scratch =
            [&backend,
             &storage,
             &make_core_range,
             &collect_core_range_pairs,
             &capture_scalar_build_stats,
             &scalar_build_stats](auto& self, segment_unit_range range) -> void {
            const std::uint64_t bp =
                static_cast<std::uint64_t>(range.core_hi_bp - range.core_lo_bp);
            constexpr std::uint64_t maximum_bounded_records_per_candidate = w + 1u;
            const std::uint64_t maximum_output_pairs =
                backend == minimizer_sketch_backend::bounded_nthash_simd
                    ? bioseq::common::saturating_mul(bp, maximum_bounded_records_per_candidate)
                    : bp;
            const std::uint64_t pair_preallocation_bytes =
                detail::admitted_vector_preallocation_bytes<pair_t>(maximum_output_pairs);
            // The strict Vulkan segment leaf returns an owned pair run
            // carrying its single scratch/result permit. Do not reserve
            // that same GPU transaction again in this caller.
            const std::uint64_t backend_scratch_bytes = 0u;
            const std::uint64_t maximum_collection_candidates =
                bioseq::common::saturating_mul(static_cast<std::uint64_t>(range.ce_u - range.cs_u),
                                               static_cast<std::uint64_t>(lpu));
            const std::uint64_t maximum_collector_sketches =
                backend == minimizer_sketch_backend::vulkan_segment
                    ? 0u
                    : (backend == minimizer_sketch_backend::bounded_nthash_simd
                           ? bioseq::common::saturating_mul(maximum_collection_candidates,
                                                            maximum_bounded_records_per_candidate)
                           : maximum_collection_candidates);
            const std::uint64_t collector_headroom_bytes =
                detail::admitted_vector_preallocation_bytes<minimap_sketch_data>(
                    maximum_collector_sketches);
            const std::uint64_t admission_bytes = bioseq::common::saturating_add(
                pair_preallocation_bytes,
                bioseq::common::saturating_add(collector_headroom_bytes, backend_scratch_bytes));
            if (backend == minimizer_sketch_backend::vulkan_segment) {
                if constexpr (requires(p_storage& s, std::uint64_t bytes) {
                                  s.demote_resident_runs_until_available(bytes);
                              }) {
                    storage.demote_resident_runs_until_available(admission_bytes);
                }
            }
            auto permit = bioseq::parallelism::memory_admission_controller::instance().try_reserve(
                admission_bytes);
            if (!permit) {
                if constexpr (requires(p_storage& s, std::uint64_t bytes) {
                                  s.demote_resident_runs_until_available(bytes);
                              }) {
                    storage.demote_resident_runs_until_available(admission_bytes);
                    permit =
                        bioseq::parallelism::memory_admission_controller::instance().try_reserve(
                            admission_bytes);
                }
            }
            if (!permit) {
                constexpr sequence_size_t k_scalar_minimum_core_bp = 4096;
                constexpr sequence_size_t k_vulkan_minimum_core_bp = 256;
                const sequence_size_t min_core_bp =
                    backend == minimizer_sketch_backend::vulkan_segment ? k_vulkan_minimum_core_bp
                                                                        : k_scalar_minimum_core_bp;
                if (range.core_hi_bp - range.core_lo_bp > min_core_bp) {
                    const sequence_size_t mid =
                        range.core_lo_bp + (range.core_hi_bp - range.core_lo_bp) / 2;
                    self(self, make_core_range(range.core_lo_bp, mid));
                    self(self, make_core_range(mid, range.core_hi_bp));
                    return;
                }
                // Never bypass the configured cap with an "emergency"
                // heap vector.  The caller can retry after pressure clears.
                throw std::bad_alloc{};
            }
            admitted_pair_run scratch;
            scratch.permit = std::move(*permit);
            if (!detail::reserve_admitted_vector_run_capacity(
                    scratch,
                    maximum_output_pairs,
                    bioseq::common::saturating_add(collector_headroom_bytes,
                                                   backend_scratch_bytes))) {
                index_corruption_panic(
                    "minimizers_per_sequence build: scratch pair capacity exceeded admission");
            }
            {
                // Declared after `scratch`: collector capacity is destroyed
                // before the shared permit can shrink or unwind.
                sketch_collector collector;
                if (maximum_collector_sketches != 0u) {
                    if (maximum_collector_sketches >
                        static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())) {
                        index_corruption_panic(
                            "minimizers_per_sequence build: collector capacity overflow");
                    }
                    collector.reserve(static_cast<std::size_t>(maximum_collector_sketches));
                }
                const std::uint64_t exact_collector_bytes = bioseq::common::saturating_mul(
                    static_cast<std::uint64_t>(collector.extract_sketches_data().capacity()),
                    static_cast<std::uint64_t>(sizeof(minimap_sketch_data)));
                const std::uint64_t live_before_collect = bioseq::common::saturating_add(
                    scratch.admitted_bytes,
                    bioseq::common::saturating_add(exact_collector_bytes, backend_scratch_bytes));
                if (exact_collector_bytes > collector_headroom_bytes ||
                    live_before_collect > scratch.permit.bytes()) {
                    index_corruption_panic(
                        "minimizers_per_sequence build: collector capacity exceeded admission");
                }
                scratch.permit.shrink_to(live_before_collect);
                collect_core_range_pairs(range.core_lo_bp,
                                         range.core_hi_bp,
                                         collector,
                                         scratch.pairs,
                                         capture_scalar_build_stats ? &scalar_build_stats
                                                                    : nullptr);
                const std::uint64_t live_after_collect = bioseq::common::saturating_add(
                    detail::admitted_vector_capacity_bytes(scratch.pairs),
                    bioseq::common::saturating_add(
                        bioseq::common::saturating_mul(
                            static_cast<std::uint64_t>(
                                collector.extract_sketches_data().capacity()),
                            static_cast<std::uint64_t>(sizeof(minimap_sketch_data))),
                        backend_scratch_bytes));
                if (live_after_collect > scratch.permit.bytes()) {
                    index_corruption_panic(
                        "minimizers_per_sequence build: collection grew beyond admission");
                }
            }
            // Backend scratch has returned; retain only exact pair capacity.
            if (!detail::finish_admitted_vector_run_transient(scratch)) {
                index_corruption_panic(
                    "minimizers_per_sequence build: scratch pair collection exceeded admission");
            }
            if (backend != minimizer_sketch_backend::vulkan_segment) {
                detail::sort_pair_run(scratch.pairs);
            }
            storage.spill_sorted_run(
                std::span<const pair_t>(scratch.pairs.data(), scratch.pairs.size()));
            scratch.release_capacity_before_permit();
        };

        auto transfer_result_runs = [&capture_scalar_build_stats,
                                     &scalar_build_stats,
                                     &storage,
                                     &collect_sort_and_spill_backend_scratch,
                                     &release_untransferred_results_noexcept](
                                        std::vector<worker_range_result>& results) {
            for (worker_range_result& result : results) {
                if (result.error) {
                    try {
                        std::rethrow_exception(result.error);
                    } catch (const exact_simd_index_runtime_error&) {
                        // Preserve the checked two_stack_nthash failure so the PG
                        // SQL boundary can translate it to 0A000. Converting
                        // it to index_corruption_panic would abort the backend
                        // and hide the original unsupported shape/status.
                        throw;
                    } catch (const packed_bounded_nthash_index_runtime_error&) {
                        // Capability and memory-pressure rejection is an
                        // operation failure, never persisted-index corruption.
                        throw;
                    } catch (const std::exception& e) {
                        const std::string message =
                            "bioseq minimizers_per_sequence worker failed for core=["
                            + std::to_string(static_cast<std::uint64_t>(
                                result.error_range.core_lo_bp))
                            + ","
                            + std::to_string(static_cast<std::uint64_t>(
                                result.error_range.core_hi_bp))
                            + "): "
                            + e.what();
                        index_corruption_panic(message);
                    } catch (...) {
                        const std::string message =
                            "bioseq minimizers_per_sequence worker failed for core=["
                            + std::to_string(static_cast<std::uint64_t>(
                                result.error_range.core_lo_bp))
                            + ","
                            + std::to_string(static_cast<std::uint64_t>(
                                result.error_range.core_hi_bp))
                            + "): non-std exception";
                        index_corruption_panic(message);
                    }
                }
                if (capture_scalar_build_stats) {
                    scalar_build_stats.merge(result.scalar_build_stats);
                }
                for (admitted_pair_run& run : result.runs.pairs) {
                    if (!run.pairs.empty()) {
                        storage.spill_sorted_run_owned_admitted(
                            std::move(run.pairs),
                            run.admitted_bytes,
                            std::move(run.permit));
                        run.admitted_bytes = 0;
                    }
                }
                for (segment_unit_range range
                     : result.deferred_for_backend_spill.pairs) {
                    collect_sort_and_spill_backend_scratch(
                        collect_sort_and_spill_backend_scratch, range);
                }
            }
            release_untransferred_results_noexcept(results);
        };

        auto collect_range_admitted =
            [&backend,
             &storage,
             &make_core_range,
             &collect_core_range_pairs,
             &capture_scalar_build_stats](
                auto& self, segment_unit_range range, worker_range_result& result) -> void {
            if (result.error) {
                return;
            }
            try {
                const std::uint64_t bp =
                    static_cast<std::uint64_t>(range.core_hi_bp - range.core_lo_bp);
                constexpr std::uint64_t maximum_bounded_records_per_candidate = w + 1u;
                const std::uint64_t maximum_output_pairs =
                    backend == minimizer_sketch_backend::bounded_nthash_simd
                        ? bioseq::common::saturating_mul(bp, maximum_bounded_records_per_candidate)
                        : bp;
                const std::uint64_t pair_preallocation_bytes =
                    detail::admitted_vector_preallocation_bytes<pair_t>(maximum_output_pairs);
                const std::uint64_t backend_scratch_bytes = 0u;
                const std::uint64_t maximum_collection_candidates = bioseq::common::saturating_mul(
                    static_cast<std::uint64_t>(range.ce_u - range.cs_u),
                    static_cast<std::uint64_t>(lpu));
                const std::uint64_t maximum_collector_sketches =
                    backend == minimizer_sketch_backend::vulkan_segment
                        ? 0u
                        : (backend == minimizer_sketch_backend::bounded_nthash_simd
                               ? bioseq::common::saturating_mul(
                                     maximum_collection_candidates,
                                     maximum_bounded_records_per_candidate)
                               : maximum_collection_candidates);
                const std::uint64_t collector_headroom_bytes =
                    detail::admitted_vector_preallocation_bytes<minimap_sketch_data>(
                        maximum_collector_sketches);
                const std::uint64_t admission_bytes = bioseq::common::saturating_add(
                    pair_preallocation_bytes,
                    bioseq::common::saturating_add(collector_headroom_bytes,
                                                   backend_scratch_bytes));
                if (backend == minimizer_sketch_backend::vulkan_segment) {
                    if constexpr (requires(p_storage& s, std::uint64_t bytes) {
                                      s.demote_resident_runs_until_available(bytes);
                                  }) {
                        storage.demote_resident_runs_until_available(admission_bytes);
                    }
                }
                auto permit =
                    bioseq::parallelism::memory_admission_controller::instance().try_reserve(
                        admission_bytes);
                if (!permit) {
                    constexpr sequence_size_t k_scalar_minimum_core_bp = 4096;
                    constexpr sequence_size_t k_vulkan_minimum_core_bp = 256;
                    const sequence_size_t min_core_bp =
                        backend == minimizer_sketch_backend::vulkan_segment
                            ? k_vulkan_minimum_core_bp
                            : k_scalar_minimum_core_bp;
                    if (range.core_hi_bp - range.core_lo_bp <= min_core_bp) {
                        if (!detail::ensure_admitted_vector_run_capacity(
                                result.deferred_for_backend_spill,
                                result.deferred_for_backend_spill.pairs.size() + 1u)) {
                            throw std::bad_alloc{};
                        }
                        result.deferred_for_backend_spill.pairs.push_back(range);
                        return;
                    }
                    const sequence_size_t mid =
                        range.core_lo_bp + (range.core_hi_bp - range.core_lo_bp) / 2;
                    self(self, make_core_range(range.core_lo_bp, mid), result);
                    self(self, make_core_range(mid, range.core_hi_bp), result);
                    return;
                }

                admitted_pair_run run{};
                run.permit = std::move(*permit);
                if (!detail::reserve_admitted_vector_run_capacity(
                        run,
                        maximum_output_pairs,
                        bioseq::common::saturating_add(collector_headroom_bytes,
                                                       backend_scratch_bytes))) {
                    index_corruption_panic(
                        "minimizers_per_sequence build: worker pair capacity exceeded admission");
                }
                {
                    sketch_collector collector;
                    if (maximum_collector_sketches != 0u) {
                        if (maximum_collector_sketches >
                            static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())) {
                            index_corruption_panic(
                                "minimizers_per_sequence build: worker collector overflow");
                        }
                        collector.reserve(static_cast<std::size_t>(maximum_collector_sketches));
                    }
                    const std::uint64_t exact_collector_bytes = bioseq::common::saturating_mul(
                        static_cast<std::uint64_t>(collector.extract_sketches_data().capacity()),
                        static_cast<std::uint64_t>(sizeof(minimap_sketch_data)));
                    const std::uint64_t live_before_collect = bioseq::common::saturating_add(
                        run.admitted_bytes,
                        bioseq::common::saturating_add(exact_collector_bytes,
                                                       backend_scratch_bytes));
                    if (exact_collector_bytes > collector_headroom_bytes ||
                        live_before_collect > run.permit.bytes()) {
                        index_corruption_panic(
                            "minimizers_per_sequence build: worker collector exceeded admission");
                    }
                    run.permit.shrink_to(live_before_collect);
                    collect_core_range_pairs(range.core_lo_bp,
                                             range.core_hi_bp,
                                             collector,
                                             run.pairs,
                                             capture_scalar_build_stats ? &result.scalar_build_stats
                                                                        : nullptr);
                    const std::uint64_t live_after_collect = bioseq::common::saturating_add(
                        detail::admitted_vector_capacity_bytes(run.pairs),
                        bioseq::common::saturating_add(
                            bioseq::common::saturating_mul(
                                static_cast<std::uint64_t>(
                                    collector.extract_sketches_data().capacity()),
                                static_cast<std::uint64_t>(sizeof(minimap_sketch_data))),
                            backend_scratch_bytes));
                    if (live_after_collect > run.permit.bytes()) {
                        index_corruption_panic("minimizers_per_sequence build: worker collection "
                                               "grew beyond admission");
                    }
                }
                // Backend scratch has returned; only vector capacity remains.
                if (!detail::finish_admitted_vector_run_transient(run)) {
                    index_corruption_panic(
                        "minimizers_per_sequence build: worker pair collection exceeded admission");
                }
                if (backend != minimizer_sketch_backend::vulkan_segment) {
                    detail::sort_pair_run(run.pairs);
                }
                detail::compact_admitted_pair_run_after_sort(run);
                if (!detail::ensure_admitted_vector_run_capacity(result.runs,
                                                                 result.runs.pairs.size() + 1u)) {
                    throw std::bad_alloc{};
                }
                result.runs.pairs.push_back(std::move(run));
            } catch (const bioseq::gpu::vulkan::vulkan_runtime_error& err) {
                const std::string_view message{err.what()};
                const bool split_admission_decline =
                    message.starts_with("Vulkan segment minimizer pair build returned invalid ");
                if (backend == minimizer_sketch_backend::vulkan_segment &&
                    split_admission_decline) {
                    constexpr sequence_size_t k_minimum_gpu_core_bp = 256;
                    if (range.core_hi_bp - range.core_lo_bp > k_minimum_gpu_core_bp) {
                        const sequence_size_t mid =
                            range.core_lo_bp + (range.core_hi_bp - range.core_lo_bp) / 2;
                        self(self, make_core_range(range.core_lo_bp, mid), result);
                        self(self, make_core_range(mid, range.core_hi_bp), result);
                        return;
                    }
                }
                result.error = std::current_exception();
                result.error_range = range;
            } catch (const std::bad_alloc&) {
                if (backend == minimizer_sketch_backend::vulkan_segment) {
                    constexpr sequence_size_t k_minimum_gpu_core_bp = 256;
                    if (range.core_hi_bp - range.core_lo_bp > k_minimum_gpu_core_bp) {
                        const sequence_size_t mid =
                            range.core_lo_bp + (range.core_hi_bp - range.core_lo_bp) / 2;
                        self(self, make_core_range(range.core_lo_bp, mid), result);
                        self(self, make_core_range(mid, range.core_hi_bp), result);
                        return;
                    }
                }
                result.error = std::current_exception();
                result.error_range = range;
            } catch (...) {
                result.error = std::current_exception();
                result.error_range = range;
            }
        };

        const std::uint64_t peak_bytes_per_fragment =
            bioseq::common::saturating_mul(
                static_cast<std::uint64_t>(frag_bp),
                static_cast<std::uint64_t>(sizeof(pair_t)));
        const std::size_t scalar_fragments_per_block = std::max<std::size_t>(
            1,
            run_buffer_bytes / std::max<std::uint64_t>(1, peak_bytes_per_fragment));
        // one GPU window's local positions must fit the 28-bit
        // field, so clamp the per-block fragment count when collecting on GPU.
        const std::size_t fragments_per_block =
            backend == minimizer_sketch_backend::vulkan_segment
                ? detail::gpu_fragments_per_block(
                    scalar_fragments_per_block,
                    frag_bp,
                    2u * window_span)
                : scalar_fragments_per_block;
        detail::admitted_vector_run<segment_unit_range> admitted_window_ranges;
        const std::size_t maximum_window_ranges =
            nfrag == 0u
                ? 0u
                : (nfrag + fragments_per_block - 1u) / fragments_per_block;
        if (!detail::ensure_admitted_vector_run_capacity(
                admitted_window_ranges, maximum_window_ranges)) {
            index_corruption_panic(
                "minimizers_per_sequence build: window metadata admission failed");
        }
        std::vector<segment_unit_range>& window_ranges =
            admitted_window_ranges.pairs;
        for (std::size_t frag_lo = 0; frag_lo < nfrag; frag_lo += fragments_per_block) {
            const std::size_t frag_hi = std::min(nfrag, frag_lo + fragments_per_block);
            const sequence_size_t core_lo = static_cast<sequence_size_t>(frag_lo) * frag_bp;
            const sequence_size_t core_hi = std::min(
                static_cast<sequence_size_t>(frag_hi) * frag_bp, total_bp);
            if (core_hi > core_lo) {
                window_ranges.push_back(make_core_range(core_lo, core_hi));
            }
        }

        auto collect_serial = [&window_ranges, &collect_range_admitted, &transfer_result_runs]() {
            detail::admitted_vector_run<worker_range_result> admitted_results;
            if (!detail::ensure_admitted_vector_run_capacity(
                    admitted_results, 1u)) {
                throw std::bad_alloc{};
            }
            admitted_results.pairs.resize(1u);
            std::vector<worker_range_result>& results = admitted_results.pairs;
            for (segment_unit_range range : window_ranges) {
                results[0] = worker_range_result{};
                collect_range_admitted(collect_range_admitted, range, results[0]);
                transfer_result_runs(results);
            }
        };

        auto& pool = bioseq::parallelism::global_pool::instance();
        // Vulkan serialises window submissions through one runtime queue. Both
        // host collectors are independent per range when the lazy scanner
        // advertises worker sharing.
        const bool may_parallel_collect =
            backend != minimizer_sketch_backend::vulkan_segment
            && bioseq::parallelism::has_parallel_tasks(pool.get_mode())
            && pool.maximum_concurrency() > 1
            && bioseq::storage::is_worker_shareable(seq)
            && nfrag
                >= bioseq::parallelism::task_parallel_minimums::k_fragment_count;

        if (!may_parallel_collect) {
            collect_serial();
        } else {
            detail::admitted_vector_run<worker_range_result>
                admitted_local_results;
            if (!detail::ensure_admitted_vector_run_capacity(
                    admitted_local_results, window_ranges.size())) {
                throw std::bad_alloc{};
            }
            admitted_local_results.pairs.resize(window_ranges.size());
            std::vector<worker_range_result>& local_results =
                admitted_local_results.pairs;
            try {
                bioseq::parallelism::parallel_for_pipeline(
                    0, local_results.size(), 1,
                    bioseq::parallelism::task_parallel_minimums::k_fragment_count,
                    bioseq::parallelism::task_parallel_region::fragment_build,
                    [&collect_range_admitted, &local_results, &window_ranges](
                            std::size_t lo, std::size_t hi) {
                        for (std::size_t rel = lo; rel < hi; ++rel) {
                            collect_range_admitted(
                                collect_range_admitted,
                                window_ranges[rel],
                                local_results[rel]);
                        }
                    });
                transfer_result_runs(local_results);
            } catch (...) {
                release_untransferred_results_noexcept(local_results);
                throw;
            }
        }

        // ---- Pass 1: k-way merge -> range-partitioned buckets + directory.
        detail::admitted_vector_run<direct_minimizer_bucket_directory_entry> admitted_dir;
        detail::admitted_vector_run<encoded_bucket_record> admitted_ob_buckets;
        detail::admitted_vector_run<std::uint64_t> admitted_ob_positions;
        std::vector<direct_minimizer_bucket_directory_entry>& dir = admitted_dir.pairs;
        std::vector<encoded_bucket_record>& ob_buckets = admitted_ob_buckets.pairs;
        std::vector<std::uint64_t>& ob_positions = admitted_ob_positions.pairs;
        std::uint64_t ob_lo = 0, ob_hi = 0;
        std::size_t ob_body_bytes = 0;                 // k_encoded_bucket_bytes*nkmers + 8*npos
        index_sub_key_t next_bucket_id = 0;
        std::uint64_t total_distinct = 0, total_positions = 0;

        auto flush_bucket = [&next_bucket_id,
                             &ob_body_bytes,
                             &storage,
                             &total_bp,
                             &ob_buckets,
                             &ob_positions,
                             &dir,
                             &ob_lo,
                             &ob_hi,
                             &admitted_dir]() {
            const index_sub_key_t sk = next_bucket_id++;
            if (sk >= k_temporary_run_sub_key_base)
                index_corruption_panic(
                    "minimizers_per_sequence: bucket id collided with reserved range");
            const std::size_t framed = k_encoded_header_bytes + ob_body_bytes;
            std::array<std::size_t,1> plan{ framed };
            storage.resize_chunks(sk, std::span<const std::size_t>(plan));
            storage.template write_to_each<char>(
                sk,
                [&framed, &total_bp, &ob_buckets, &ob_positions](
                    std::size_t c, char* outbuf, std::size_t cap) {
                    DEBUG_ASSERT(c == 0 && cap >= framed, "one bucket blob fits one chunk");
                    detail::serialize_direct_minimizer_bucket_image(
                        outbuf, total_bp, ob_buckets, ob_positions);
                    return c;
                });
            DEBUG_ASSERT(dir.empty() || dir.back().hi_kmer < ob_lo, "directory not strictly ascending (flush)");
            if (!detail::ensure_admitted_vector_run_capacity(
                    admitted_dir, dir.size() + 1u)) {
                index_corruption_panic(
                    "minimizers_per_sequence: directory admission failed");
            }
            dir.push_back({ ob_lo, ob_hi, sk });
            ob_buckets.clear();
            ob_positions.clear();
            ob_body_bytes = 0;
            ob_lo = 0;
            ob_hi = 0;
        };

        storage.merge_runs([&total_distinct,
                            &total_positions,
                            &storage,
                            &next_bucket_id,
                            &flush_bucket,
                            &total_bp,
                            &admitted_ob_positions,
                            &admitted_ob_buckets,
                            &ob_positions,
                            &ob_buckets,
                            &ob_body_bytes,
                            &ob_lo,
                            &ob_hi](std::uint64_t kmer_key, auto&& positions) {
            const std::uint64_t pcount = positions.total_count();
            if (pcount > UINT32_MAX)
                index_corruption_panic("merge_runs: kmer count > u32 (encoded_bucket.pos_count)");
            ++total_distinct;
            total_positions += pcount;
            const std::size_t kmer_framed = k_encoded_header_bytes + k_encoded_bucket_bytes * 1 +
                                            sizeof(std::uint64_t) * pcount;
            if (kmer_framed > k_bucket_target_bytes) {
                // Split the heavy-hitter key across standalone direct-bucket images.
                if (!ob_buckets.empty())
                    flush_bucket();
                const index_sub_key_t sk = next_bucket_id++;
                if (sk >= k_temporary_run_sub_key_base)
                    index_corruption_panic(
                        "minimizers_per_sequence: bucket id collided with reserved range");
                detail::admitted_vector_run<std::uint64_t> admitted_kpos;
                if (!detail::ensure_admitted_vector_run_capacity(
                        admitted_kpos, static_cast<std::size_t>(pcount))) {
                    index_corruption_panic(
                        "minimizers_per_sequence: heavy positions admission failed");
                }
                std::vector<std::uint64_t>& kpos = admitted_kpos.pairs;
                positions.for_each_batch([&kpos](std::span<const std::uint64_t> b) {
                    if (b.size()
                            > (std::numeric_limits<std::size_t>::max)()
                                - kpos.size()) {
                        index_corruption_panic(
                            "minimizers_per_sequence: heavy positions overflow");
                    }
                    kpos.insert(kpos.end(), b.begin(), b.end());
                });
                const std::size_t per = (k_bucket_target_bytes - k_encoded_header_bytes - k_encoded_bucket_bytes) / sizeof(std::uint64_t);
                const std::size_t plan_count =
                    (static_cast<std::size_t>(pcount) + per - 1u) / per;
                detail::admitted_vector_run<std::size_t> admitted_plan;
                if (!detail::ensure_admitted_vector_run_capacity(
                        admitted_plan, plan_count)) {
                    index_corruption_panic(
                        "minimizers_per_sequence: heavy plan admission failed");
                }
                std::vector<std::size_t>& plan = admitted_plan.pairs;
                for (std::size_t off = 0; off < static_cast<std::size_t>(pcount); off += per) {
                    plan.push_back(
                        k_encoded_header_bytes + k_encoded_bucket_bytes +
                        sizeof(std::uint64_t) *
                            std::min<std::size_t>(per, static_cast<std::size_t>(pcount) - off));
                }
                storage.resize_chunks(sk, std::span<const std::size_t>(plan));
                storage.template write_to_each<char>(
                    sk,
                    [&per, &pcount, &kmer_key, &total_bp, &kpos](
                        std::size_t c, char* outbuf, std::size_t) {
                        const std::size_t start = c * per;
                        const std::size_t cnt =
                            std::min<std::size_t>(per, static_cast<std::size_t>(pcount) - start);
                        encoded_bucket_record rec{kmer_key, 0u, static_cast<std::uint32_t>(cnt)};
                        detail::serialize_direct_minimizer_bucket_image(
                            outbuf,
                            total_bp,
                            std::span<const encoded_bucket_record>(&rec, 1),
                            std::span<const std::uint64_t>(kpos.data() + start, cnt));
                        return c;
                    });
                DEBUG_ASSERT(dir.empty() || dir.back().hi_kmer < kmer_key, "directory not strictly ascending (heavy-hitter)");
                if (!detail::ensure_admitted_vector_run_capacity(
                        admitted_dir, dir.size() + 1u)) {
                    index_corruption_panic(
                        "minimizers_per_sequence: directory admission failed");
                }
                dir.push_back({ kmer_key, kmer_key, sk });
            } else {
                const std::size_t add = k_encoded_bucket_bytes + sizeof(std::uint64_t) * pcount;
                if (k_encoded_header_bytes + ob_body_bytes + add > k_bucket_target_bytes &&
                    !ob_buckets.empty())
                    flush_bucket();
                if (!detail::ensure_admitted_vector_run_capacity(
                        admitted_ob_positions,
                        ob_positions.size() + static_cast<std::size_t>(pcount))
                    || !detail::ensure_admitted_vector_run_capacity(
                        admitted_ob_buckets, ob_buckets.size() + 1u)) {
                    index_corruption_panic(
                        "minimizers_per_sequence: bucket assembly admission failed");
                }
                const std::uint32_t off = static_cast<std::uint32_t>(ob_positions.size());
                positions.for_each_batch([&ob_positions](std::span<const std::uint64_t> b) {
                    ob_positions.insert(ob_positions.end(), b.begin(), b.end());
                });
                ob_buckets.push_back(
                    encoded_bucket_record{kmer_key, off, static_cast<std::uint32_t>(pcount)});
                ob_hi = kmer_key;
                if (ob_buckets.size() == 1)
                    ob_lo = kmer_key;
                ob_body_bytes += add;
            }
        });
        if (!ob_buckets.empty())
            flush_bucket();

        // ---- write directory LAST.
        {
            const std::size_t dir_bytes =
                sizeof(direct_minimizer_bucket_directory) +
                sizeof(direct_minimizer_bucket_directory_entry) * dir.size();
            std::array<std::size_t, 1> plan{dir_bytes};
            storage.resize_chunks(k_directory_sub_key, std::span<const std::size_t>(plan));
            storage.template write_to_each<char>(
                k_directory_sub_key,
                [&dir, &total_distinct, &total_positions, &backend](
                    std::size_t c, char* outbuf, std::size_t) {
                    direct_minimizer_bucket_directory h;
                    h.magic = direct_minimizer_bucket_directory::k_direct_minimizer_bucket_magic;
                    h.layout_revision = direct_minimizer_bucket_directory::
                        k_direct_minimizer_bucket_layout_revision;
                    h.flags = backend == minimizer_sketch_backend::vulkan_segment &&
                                      !std::same_as<p_alphabet, ::seq::dna4>
                                  ? direct_minimizer_bucket_directory::k_gpu_built
                                  : 0u;
                    h.num_buckets = dir.size();
                    h.total_distinct_kmers = total_distinct;
                    h.total_positions = total_positions;
                    std::memcpy(outbuf, &h, sizeof(h));
                    if (!dir.empty())
                        std::memcpy(outbuf + sizeof(h),
                                    dir.data(),
                                    sizeof(direct_minimizer_bucket_directory_entry) * dir.size());
                    return c;
                });
        }

        // ---- Pass 2: cleanup.
        if constexpr (requires (p_storage& s) { s.snapshot_sorted_run_stats_before_release(); }) {
            storage.snapshot_sorted_run_stats_before_release();
        }
        storage.release_runs();
        if (capture_scalar_build_stats) {
            scalar_build_stats.record_completed_build();
            *scalar_build_stats_out = scalar_build_stats;
        }
    }

    template <sequence_alphabet p_alphabet>
    static void build(
        p_storage& storage,
        bioseq::storage::lazy_sequence_view<p_alphabet> seq,
        minimizer_sketch_backend backend,
        const bioseq::minimizers::minimizer_kernels* kernels,
        const std::uint8_t alphabet_code) {
        build(storage, seq, backend, kernels, nullptr, alphabet_code);
    }

    // scalar-default overload delegates to the explicit backend.
    template <sequence_alphabet p_alphabet>
    static void build(
            p_storage& storage,
            bioseq::storage::lazy_sequence_view<p_alphabet> seq) {
        build(storage, seq, minimizer_sketch_backend::scalar, nullptr);
    }

    template <sequence_alphabet p_alphabet>
    static void rebuild(
        p_storage& storage,
        bioseq::storage::lazy_sequence_view<p_alphabet> seq,
        minimizer_sketch_backend backend,
        const bioseq::minimizers::minimizer_kernels* kernels,
        std::uint8_t alphabet_code =
            bioseq::minimizers::k_unbound_packed_alphabet_code)
    {
        if (backend == minimizer_sketch_backend::exact_simd
            && (kernels == nullptr || kernels->two_stack_nthash == nullptr)) {
            throw exact_simd_index_runtime_error{
                exact_simd_index_status::invalid_state,
                "minimizers_per_sequence rebuild"};
        }
        if (backend == minimizer_sketch_backend::bounded_nthash_simd
            && (kernels == nullptr || kernels->bounded_nthash == nullptr)) {
            throw packed_bounded_nthash_index_runtime_error{
                packed_bounded_nthash_index_status::invalid_state,
                "minimizers_per_sequence rebuild"};
        }
        storage.clear_entity();
        build(storage, seq, backend, kernels, nullptr, alphabet_code);
    }

    template <sequence_alphabet p_alphabet>
    static void rebuild(
            p_storage& storage,
            bioseq::storage::lazy_sequence_view<p_alphabet> seq) {
        rebuild(storage, seq, minimizer_sketch_backend::scalar, nullptr);
    }

    /// query-time: load ONLY the buckets containing any query kmer (§10.5).
    static minimizer_hashmap_fragment_index assemble_for_query(
            p_storage& storage, std::span<const minimap_sketch_data> query_minimizers,
            sequence_size_t target_len, std::int32_t max_occ,
            minimizer_assembly_scratch& scratch) {
        return assemble_for_query_segment(
            storage,
            query_minimizers,
            target_len,
            max_occ,
            0,
            target_len,
            scratch);
    }

    /// query-time: load query-touched buckets but keep only positions in one
    /// target collection range. Occurrence filtering remains global.
    static minimizer_hashmap_fragment_index assemble_for_query_segment(
            p_storage& storage, std::span<const minimap_sketch_data> query_minimizers,
            sequence_size_t target_len, std::int32_t max_occ,
            sequence_size_t clip_lo_bp, sequence_size_t clip_hi_bp,
            minimizer_assembly_scratch& scratch) {
        using detail::k_encoded_header_bytes; using detail::k_encoded_bucket_bytes;
        scratch.clear_query();
        clip_hi_bp = std::min(clip_hi_bp, target_len);
        if (clip_lo_bp > clip_hi_bp) {
            clip_lo_bp = clip_hi_bp;
        }
        // 1. read the directory into an owned buffer (the directory view copies it).
        const char* dblob = storage.template read_chunk<char>(k_directory_sub_key, 0);
        const std::size_t dbytes = storage.chunk_byte_size(k_directory_sub_key, 0);
        storage.validate_minimizer_directory_or_error(dblob, dbytes);
        direct_minimizer_bucket_directory_view dir{ dblob, dbytes };
        const std::uint32_t max_occ_threshold =
            detail::effective_max_occ_threshold(max_occ, dir.is_gpu_built());
        // 2. collect distinct query kmers + the distinct bucket sub_keys they touch.
        auto& qkmers = scratch.query_kmers;
        auto& wanted = scratch.wanted_sub_keys;
        for (const auto& qm : query_minimizers) {
            qkmers.push_back(qm.kmer());
            if (const direct_minimizer_bucket_directory_entry* e = dir.find(qm.kmer())) wanted.push_back(e->sub_key);
        }
        auto sort_unique = [](auto& v){ std::sort(v.begin(), v.end()); v.erase(std::unique(v.begin(), v.end()), v.end()); };
        sort_unique(qkmers);
        sort_unique(wanted);
        // 3. read only those buckets; deserialise; drop kmers with global count > max_occ.
        auto& pairs = scratch.pairs;
        auto emit_kept =
            [&qkmers, &max_occ_threshold, &pairs, &clip_lo_bp, &clip_hi_bp](
                std::uint64_t kmer, std::uint32_t pos_count, const std::uint64_t* gpos) {
                if (!std::binary_search(qkmers.begin(), qkmers.end(), kmer))
                    return; // clip to QUERY kmers
                if (pos_count > max_occ_threshold)
                    return; // drop over-frequent kmer
                for (std::uint32_t j = 0; j < pos_count; ++j) {
                    const sequence_size_t pos_bp = static_cast<sequence_size_t>(gpos[j] >> 1);
                    if (pos_bp < clip_lo_bp || pos_bp >= clip_hi_bp) {
                        continue;
                    }
                    pairs.emplace_back(kmer, gpos[j]);
                }
            };
        for (std::int64_t sk : wanted) {
            const std::size_t nc = storage.number_of_chunks(sk);
            if (nc == 0) storage.index_error("minimizers_per_sequence: directory references a bucket with no chunks");
            if (nc > 1) {
                // A heavy-hitter bucket contains one k-mer; metadata gives its exact count.
                std::size_t exact = 0;
                for (std::size_t c = 0; c < nc; ++c) {
                    storage.validate_minimizer_chunk_metadata_or_error(sk, c, k_encoded_header_bytes + k_encoded_bucket_bytes, alignof(std::uint64_t));
                    const std::size_t cb = storage.chunk_byte_size(sk, c);
                    exact += (cb - k_encoded_header_bytes - k_encoded_bucket_bytes) / sizeof(std::uint64_t);
                }
                if (exact > static_cast<std::size_t>(max_occ_threshold)) continue;  // drop over-frequent kmer
                for (std::size_t c = 0; c < nc; ++c) {             // materialise EVERY chunk
                    const std::size_t cb = storage.chunk_byte_size(sk, c);
                    const char*       p  = storage.template read_chunk<char>(sk, c);
                    const auto h = storage.validate_minimizer_blob_or_error(p, cb, 1);
                    if (h.bucket_count != 1)
                        storage.index_error("assemble_for_query: multi-chunk bucket with bucket_count!=1 (byte-math invalid)");
                    wire_for_each_bucket(p, cb, scratch.bucket_positions, emit_kept);
                }
                continue;
            }
            const std::size_t cb0 = storage.chunk_byte_size(sk, 0);
            const char*       p0  = storage.template read_chunk<char>(sk, 0);
            storage.validate_minimizer_blob_or_error(p0, cb0);
            wire_for_each_bucket(p0, cb0, scratch.bucket_positions, emit_kept);
        }
        // 4. build the single pipeline IndexT.
        minimizer_hashmap_fragment_index idx; idx.build_from_pairs(pairs, target_len);
        return idx;
    }
};

}  // namespace bioseq::indexing
export namespace bioseq::indexing {
template <> struct index_descriptor<direct_minimizer_bucket_index> {
    static constexpr sequence_index_layout index_type = sequence_index_layout::fragment_chunks;
    static constexpr std::string_view  index_name = k_minimizers_per_sequence_index_name;
    static constexpr bool allows_update_on_insert = true;   // opt-in external sort; not a fresh-table default
};
static_assert(requires { index_descriptor<direct_minimizer_bucket_index>::index_name; },
              "direct_minimizer_bucket_index index_descriptor must be exported");
}
