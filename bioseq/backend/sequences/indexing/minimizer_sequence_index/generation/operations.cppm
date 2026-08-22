/**
 * @file minimizer_sequence_index/generation/operations.cppm
 * @brief Validated core collection, sealed spill replay, and grouped-SoA generation.
 */
module;

#include <libassert/assert.hpp>

export module bioseq.indexing.minimizer_sequence.generation:operations;

import std;
import :replay_contract;
import bioseq.indexing.minimizer_sequence.admitted_runs;
import bioseq.indexing.minimizer_sequence.bucket_codec;
import bioseq.common.checked_arithmetic;
import bioseq.gpu.vulkan.abi;
import bioseq.indexing.minimizer_hashmap;
import bioseq.indexing.minimizer_index;
import bioseq.indexing.storage;
import bioseq.indexing.type;
import bioseq.parallelism.memory_admission;
import bioseq.sequences;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.highway_minimizer_contract;
import bioseq.sequences.kmers.vulkan_minimizers;
import bioseq.storage.lazy_sequence_view;
import bioseq.storage.size;

export namespace bioseq::indexing {

using bioseq::minimizers::minimap_sketch_data;
using bioseq::minimizers::minimizer_sketch_backend;
using bioseq::minimizers::sketch_collector;
using bioseq::storage::sequence_size_t;

/**
 * @brief Owns generation-only spill and replay operations for a direct minimizer index.
 * @tparam p_storage Storage satisfying the fragment-chunk and external-spill contracts
 *
 * Keeping generation independent from query assembly lets persistence code import the
 * exact producer contract without loading the complete direct-index facade.
 */
template <class p_storage> struct direct_minimizer_generation_replay {
    static_assert(is_index_chunked_storage_for<p_storage, sequence_index_layout::fragment_chunks>);
    static_assert(is_external_sort_spill_storage<p_storage>);

    /**
     * @brief Move-only authority for one qualified set of external spill runs.
     */
    class generation_spill_result {
      private:
        friend struct direct_minimizer_generation_replay<p_storage>;

        std::uint64_t run_count_ = 0;
        exact_simd_index_execution_stats exact_simd_stats_{};
        bool exact_simd_stats_available_ = false;
        minimizer_scheme_identity minimizer_scheme_{};
        std::uint64_t target_length_ = 0;
        std::uint64_t position_segment_span_bp_ = 0;
        minimizer_sequence_generation_execution_receipt execution_receipt_{};
        std::shared_ptr<const direct_minimizer_spill_authority<p_storage>> authority_{};

        /** @brief Constructs one sealed spill result from qualification outputs. */
        generation_spill_result(
            std::uint64_t run_count,
            const exact_simd_index_execution_stats& exact_simd_stats,
            bool exact_simd_stats_available,
            minimizer_scheme_identity minimizer_scheme,
            std::uint64_t target_length,
            std::uint64_t position_segment_span_bp,
            const minimizer_sequence_generation_execution_receipt& execution_receipt,
            std::shared_ptr<const direct_minimizer_spill_authority<p_storage>> authority) noexcept
            : run_count_(run_count), exact_simd_stats_(exact_simd_stats),
              exact_simd_stats_available_(exact_simd_stats_available),
              minimizer_scheme_(minimizer_scheme), target_length_(target_length),
              position_segment_span_bp_(position_segment_span_bp),
              execution_receipt_(execution_receipt), authority_(std::move(authority)) {}

        /** @brief Clears all spill authority so the result cannot be reused. */
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

        /** @brief Moves spill fields out of another result and poisons the donor. */
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
        /** @brief Default construction is forbidden for a sealed spill handle. */
        generation_spill_result() = delete;
        /** @brief Copying would duplicate move-only spill authority. */
        generation_spill_result(const generation_spill_result&) = delete;
        /** @brief Assignment would duplicate move-only spill authority. */
        generation_spill_result& operator=(const generation_spill_result&) = delete;
        /** @brief Move-constructs by stealing the donor spill authority. */
        generation_spill_result(generation_spill_result&& other) noexcept {
            steal_from_(std::move(other));
        }
        /** @brief Move-assigns by poisoning this result and stealing the donor. */
        generation_spill_result& operator=(generation_spill_result&& other) noexcept {
            if (this != &other) {
                poison_();
                steal_from_(std::move(other));
            }
            return *this;
        }

        /** @brief Returns the number of external spill runs sealed in storage. */
        [[nodiscard]] std::uint64_t run_count() const noexcept {
            return run_count_;
        }
        /** @brief Returns exact-SIMD execution stats accumulated during spill. */
        [[nodiscard]] const exact_simd_index_execution_stats& exact_simd_stats() const noexcept {
            return exact_simd_stats_;
        }
        /** @brief Returns whether exact-SIMD stats were recorded during spill. */
        [[nodiscard]] bool exact_simd_stats_available() const noexcept {
            return exact_simd_stats_available_;
        }
        /** @brief Returns the minimizer scheme identity used for spill qualification. */
        [[nodiscard]] minimizer_scheme_identity minimizer_scheme() const noexcept {
            return minimizer_scheme_;
        }
        /** @brief Returns the target sequence length covered by the spill. */
        [[nodiscard]] std::uint64_t target_length() const noexcept {
            return target_length_;
        }
        /** @brief Returns the position-segment span used during spill and replay. */
        [[nodiscard]] std::uint64_t position_segment_span_bp() const noexcept {
            return position_segment_span_bp_;
        }
        /** @brief Returns the sealed generation execution receipt for the spill. */
        [[nodiscard]] const minimizer_sequence_generation_execution_receipt&
        execution_receipt() const noexcept {
            return execution_receipt_;
        }
    };

    /**
     * @brief Compute the source range and halo for one generation core.
     */
    template <sequence_alphabet p_alphabet>
    [[nodiscard]] static inline std::expected<segment_unit_range,
                                              minimizer_generation_encoding_error>
    make_generation_collection_range(bioseq::storage::lazy_sequence_view<p_alphabet> sequence,
                                     std::uint64_t core_lo,
                                     std::uint64_t core_hi,
                                     std::uint64_t halo_bp = 10u + 15u - 2u) noexcept {
        const std::uint64_t target_length = sequence.bp_size();
        if (core_lo > core_hi || core_hi > target_length) {
            return std::unexpected(minimizer_generation_encoding_error::bp_range);
        }
        const std::uint64_t collection_lo = core_lo >= halo_bp ? core_lo - halo_bp : 0u;
        const std::uint64_t collection_hi =
            core_hi > target_length - std::min(target_length, halo_bp) ? target_length
                                                                       : core_hi + halo_bp;
        if (collection_lo > (std::numeric_limits<sequence_size_t>::max)() ||
            collection_hi > (std::numeric_limits<sequence_size_t>::max)()) {
            return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
        }
        return segment_unit_range{
            .cs_u = static_cast<sequence_size_t>(collection_lo),
            .ce_u = static_cast<sequence_size_t>(collection_hi),
            .core_lo_bp = static_cast<sequence_size_t>(core_lo),
            .core_hi_bp = static_cast<sequence_size_t>(core_hi),
        };
    }

    /**
     * @brief Collect one generation core with the selected backend.
     */
    template <sequence_alphabet p_alphabet>
    [[nodiscard]] static inline std::expected<exact_simd_index_execution_stats,
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
        if ((backend == minimizer_sketch_backend::exact_simd &&
             (kernels == nullptr || kernels->two_stack_nthash == nullptr)) ||
            (backend == minimizer_sketch_backend::bounded_nthash_simd &&
             (kernels == nullptr || kernels->bounded_nthash == nullptr))) {
            return std::unexpected(minimizer_generation_encoding_error::storage_contract_mismatch);
        }
        if (range.core_lo_bp > range.core_hi_bp || range.core_hi_bp > target_length ||
            range.cs_u > range.ce_u || range.ce_u > target_length) {
            return std::unexpected(minimizer_generation_encoding_error::bp_range);
        }
        if (range.core_lo_bp == range.core_hi_bp || range.cs_u == range.ce_u) {
            return exact_simd_index_execution_stats{};
        }
        const std::uint64_t emit_base = range.cs_u;
        constexpr std::uint64_t halo_bp = 10u + 15u - 2u;
        const std::uint64_t collection_hi =
            range.core_hi_bp > target_length - std::min(target_length, halo_bp)
                ? target_length
                : range.core_hi_bp + halo_bp;
        const std::uint64_t local_bp_size =
            collection_hi > emit_base ? collection_hi - emit_base : 0u;
        constexpr std::uint64_t minimum_work_bases = 10u + 15u - 1u;
        /** @brief Increments one optional execution-receipt route counter with saturation. */
        const auto add_route =
            [execution_receipt](
                std::uint64_t minimizer_sequence_generation_execution_receipt::* member,
                std::uint64_t increment = 1u) noexcept {
                if (execution_receipt != nullptr) {
                    minimizer_sequence_generation_execution_receipt::add(
                        execution_receipt->*member, increment, execution_receipt->saturated);
                }
            };
        if (local_bp_size < minimum_work_bases) {
            add_route(&minimizer_sequence_generation_execution_receipt::short_core_runs);
            return exact_simd_index_execution_stats{};
        }
        if (backend == minimizer_sketch_backend::vulkan_segment) {
            add_route(&minimizer_sequence_generation_execution_receipt::vulkan_core_runs);
            bioseq::gpu::vulkan::vulkan_segment_pair_execution_stats core_stats{};
            auto pairs =
                bioseq::gpu::vulkan::collect_sorted_minimizer_pairs_vulkan_segment_or_throw(
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
                execution_receipt->saturated =
                    execution_receipt->saturated || execution_receipt->vulkan_stats.saturated;
            }
            output.insert(output.end(),
                          std::make_move_iterator(pairs.pairs.begin()),
                          std::make_move_iterator(pairs.pairs.end()));
            return exact_simd_index_execution_stats{};
        }
        if (backend != minimizer_sketch_backend::scalar &&
            backend != minimizer_sketch_backend::exact_simd &&
            backend != minimizer_sketch_backend::bounded_nthash_simd) {
            return std::unexpected(minimizer_generation_encoding_error::storage_contract_mismatch);
        }

        collector.clear();
        exact_simd_index_execution_stats exact_simd_stats{};
        if (backend == minimizer_sketch_backend::exact_simd) {
            add_route(&minimizer_sequence_generation_execution_receipt::exact_simd_core_runs);
            exact_simd_stats =
                collect_exact_simd_index_range_or_throw(collector,
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
                execution_receipt->exact_simd_stats.merge(exact_simd_stats);
                execution_receipt->exact_simd_stats_available = true;
                execution_receipt->saturated =
                    execution_receipt->saturated || execution_receipt->exact_simd_stats.saturated;
            }
        } else if (backend == minimizer_sketch_backend::bounded_nthash_simd) {
            if (!allow_packed_bounded) {
                return std::unexpected(
                    minimizer_generation_encoding_error::storage_contract_mismatch);
            }
            add_route(&minimizer_sequence_generation_execution_receipt::bounded_nthash_core_runs);
            std::uint64_t materialized_window_bp = 0u;
            const auto bounded_result =
                collect_packed_bounded_nthash_index_lazy(sequence,
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
                execution_receipt->peak_bounded_materialized_window_bp = std::max(
                    execution_receipt->peak_bounded_materialized_window_bp, materialized_window_bp);
                execution_receipt->packed_bounded_status = bounded_result.status;
                execution_receipt->packed_bounded_stats.merge(bounded_result.stats);
                execution_receipt->packed_bounded_stats_available = true;
                execution_receipt->saturated = execution_receipt->saturated ||
                                               execution_receipt->packed_bounded_stats.saturated;
            }
            if (bounded_result.status != packed_bounded_nthash_index_status::ok) {
                throw packed_bounded_nthash_index_runtime_error{bounded_result.status,
                                                                "minimizers_per_sequence build"};
            }
        } else {
            add_route(&minimizer_sequence_generation_execution_receipt::scalar_core_runs);
            bioseq::simd::collect_minimizers_segment_scalar<minimap_sketch_data>(
                collector, sequence, range.cs_u, range.ce_u, 10u, 15u);
            if (scalar_build_stats != nullptr) {
                scalar_build_stats->record_scalar_collection(
                    range.ce_u - range.cs_u, range.ce_u - range.cs_u, 1u, 1u);
            }
        }
        for (const minimap_sketch_data& minimizer : collector.extract_sketches_data()) {
            const std::uint64_t position =
                emit_base + static_cast<std::uint64_t>(minimizer.last_pos());
            if (position < range.core_lo_bp || position >= range.core_hi_bp) {
                continue;
            }
            output.emplace_back(minimizer.kmer(),
                                (position << 1u) | static_cast<std::uint64_t>(minimizer.strand()));
        }
        return exact_simd_stats;
    }

    /// Prepare external runs serially within the bounded production budget. Each source core is
    /// collected and spilled before the next is allocated. The returned direct minimizer-bucket
    /// producer reopens these same external runs for both encoder passes.

    /**
     * @brief Collect and seal external replay runs within the storage admission budget.
     */
    template <sequence_alphabet p_alphabet>
    [[nodiscard]] static inline std::expected<generation_spill_result,
                                              minimizer_generation_encoding_error>
    spill_generation_replay_runs_serial(
        p_storage& storage,
        bioseq::storage::lazy_sequence_view<p_alphabet> sequence,
        minimizer_sketch_backend backend,
        const bioseq::minimizers::minimizer_kernels* kernels,
        std::uint64_t core_span_bp = 0,
        std::uint64_t position_segment_span_bp = k_minimizer_preferred_segment_span_bp,
        minimizer_sequence_generation_execution_receipt* execution_receipt_out = nullptr,
        std::uint8_t alphabet_code = bioseq::minimizers::k_unbound_packed_alphabet_code) {
        static_assert(direct_minimizer_qualified_spill_storage<p_storage>);
        constexpr std::uint64_t default_core_span = 32'768u;
        constexpr std::uint64_t lpu = 1u;
        const std::uint64_t target_length = sequence.bp_size();
        struct receipt_output_guard {
            minimizer_sequence_generation_execution_receipt* output = nullptr;
            minimizer_sequence_generation_execution_receipt receipt{};
            /** @brief Writes the accumulated receipt to the caller output on scope exit. */
            ~receipt_output_guard() {
                if (output != nullptr) {
                    *output = receipt;
                }
            }
        } receipt_guard;
        receipt_guard.output = execution_receipt_out;
        minimizer_sequence_generation_execution_receipt& receipt = receipt_guard.receipt;
        receipt.selected_backend = backend;
        receipt.minimizer_scheme = minimizer_scheme_identity_for_backend(backend);
        receipt.target_length = target_length;
        receipt.window_size = 10;
        receipt.kmer_size = 15;
        receipt.configured_core_span_bp = core_span_bp == 0 ? default_core_span : core_span_bp;
        if ((backend == minimizer_sketch_backend::exact_simd &&
             (kernels == nullptr || kernels->two_stack_nthash == nullptr)) ||
            (backend == minimizer_sketch_backend::bounded_nthash_simd &&
             (kernels == nullptr || kernels->bounded_nthash == nullptr))) {
            return std::unexpected(minimizer_generation_encoding_error::storage_contract_mismatch);
        }
        if (core_span_bp == 0) {
            core_span_bp = default_core_span;
        }
        if (core_span_bp == 0 || core_span_bp > default_core_span ||
            position_segment_span_bp == 0) {
            return std::unexpected(minimizer_generation_encoding_error::segment_span);
        }
        const minimizer_scheme_identity minimizer_scheme =
            minimizer_scheme_identity_for_backend(backend);
        if (!is_supported_minimizer_scheme_identity(minimizer_scheme.id,
                                                    minimizer_scheme.revision)) {
            return std::unexpected(minimizer_generation_encoding_error::storage_contract_mismatch);
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
            /** @brief Grows replay buffers under the shared memory admission permit. */
            const auto ensure_replay_capacity =
                [&buffers](std::uint64_t expected_run, std::uint64_t expected_sketches) -> bool {
                constexpr std::uint64_t max = (std::numeric_limits<std::uint64_t>::max)();
                if (expected_run >
                        static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)()) ||
                    expected_sketches >
                        static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())) {
                    return false;
                }
                const std::uint64_t run_bytes = bioseq::common::saturating_mul(
                    static_cast<std::uint64_t>(buffers.run.capacity()),
                    static_cast<std::uint64_t>(sizeof(detail::sorted_pair_t)));
                const std::uint64_t sketch_bytes = bioseq::common::saturating_mul(
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
                                  static_cast<std::uint64_t>(sizeof(detail::sorted_pair_t))),
                              bioseq::parallelism::k_vector_preallocation_headroom_factor)
                        : 0u;
                const std::uint64_t sketch_headroom =
                    buffers.collector.extract_sketches_data().capacity() < expected_sketches
                        ? bioseq::common::saturating_mul(
                              bioseq::common::saturating_mul(
                                  expected_sketches,
                                  static_cast<std::uint64_t>(sizeof(minimap_sketch_data))),
                              bioseq::parallelism::k_vector_preallocation_headroom_factor)
                        : 0u;
                const std::uint64_t allocation_peak = bioseq::common::saturating_add(
                    retained_bytes, bioseq::common::saturating_add(run_headroom, sketch_headroom));
                if (run_bytes == max || sketch_bytes == max || retained_bytes == max ||
                    run_headroom == max || sketch_headroom == max || allocation_peak == max) {
                    return false;
                }
                if (buffers.permit) {
                    if (buffers.permit.bytes() < retained_bytes ||
                        !buffers.permit.grow_to(allocation_peak)) {
                        return false;
                    }
                } else {
                    if (retained_bytes != 0u) {
                        return false;
                    }
                    auto permit =
                        bioseq::parallelism::memory_admission_controller::instance().try_reserve(
                            allocation_peak);
                    if (!permit) {
                        return false;
                    }
                    buffers.permit = std::move(*permit);
                }
                if (buffers.run.capacity() < expected_run) {
                    buffers.run.reserve(static_cast<std::size_t>(expected_run));
                }
                if (buffers.collector.extract_sketches_data().capacity() < expected_sketches) {
                    buffers.collector.reserve(static_cast<std::size_t>(expected_sketches));
                }
                const std::uint64_t exact_run = bioseq::common::saturating_mul(
                    static_cast<std::uint64_t>(buffers.run.capacity()),
                    static_cast<std::uint64_t>(sizeof(detail::sorted_pair_t)));
                const std::uint64_t exact_sketch = bioseq::common::saturating_mul(
                    static_cast<std::uint64_t>(
                        buffers.collector.extract_sketches_data().capacity()),
                    static_cast<std::uint64_t>(sizeof(minimap_sketch_data)));
                const std::uint64_t exact = bioseq::common::saturating_add(exact_run, exact_sketch);
                if (exact_run == max || exact_sketch == max || exact == max ||
                    exact > buffers.permit.bytes()) {
                    return false;
                }
                buffers.permit.shrink_to(exact);
                return true;
            };

            while (segment_lo < target_length) {
                if (!sql_integer_can_represent(segment_ordinal)) {
                    return std::unexpected(minimizer_generation_encoding_error::segment_count);
                }
                const std::uint64_t segment_hi =
                    segment_lo + std::min(position_segment_span_bp, target_length - segment_lo);
                for (std::uint64_t core_lo = segment_lo; core_lo < segment_hi;) {
                    const std::uint64_t core_hi =
                        core_lo + std::min(core_span_bp, segment_hi - core_lo);
                    auto range = make_generation_collection_range(sequence, core_lo, core_hi);
                    if (!range) {
                        return std::unexpected(range.error());
                    }
                    receipt.peak_core_span_bp =
                        std::max(receipt.peak_core_span_bp, core_hi - core_lo);
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
                    constexpr std::uint64_t maximum_bounded_records_per_candidate = 10u + 1u;
                    const std::uint64_t expected_sketches =
                        backend == minimizer_sketch_backend::vulkan_segment
                            ? 0u
                            : (backend == minimizer_sketch_backend::bounded_nthash_simd
                                   ? bioseq::common::saturating_mul(
                                         maximum_new_sketches,
                                         maximum_bounded_records_per_candidate)
                                   : maximum_new_sketches);
                    const std::uint64_t expected_run =
                        backend == minimizer_sketch_backend::vulkan_segment ? core_hi - core_lo
                                                                            : expected_sketches;
                    if (!ensure_replay_capacity(expected_run, expected_sketches)) {
                        return std::unexpected(minimizer_generation_encoding_error::admission);
                    }
                    auto collected = collect_generation_core_run(sequence,
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
                    const std::uint64_t retained_after_collection = bioseq::common::saturating_add(
                        bioseq::common::saturating_mul(
                            static_cast<std::uint64_t>(buffers.run.capacity()),
                            static_cast<std::uint64_t>(sizeof(detail::sorted_pair_t))),
                        bioseq::common::saturating_mul(
                            static_cast<std::uint64_t>(
                                buffers.collector.extract_sketches_data().capacity()),
                            static_cast<std::uint64_t>(sizeof(minimap_sketch_data))));
                    if (retained_after_collection == (std::numeric_limits<std::uint64_t>::max)() ||
                        retained_after_collection > buffers.permit.bytes()) {
                        return std::unexpected(minimizer_generation_encoding_error::admission);
                    }
                    buffers.permit.shrink_to(retained_after_collection);
                    detail::sort_pair_run(buffers.run);
                    // A source occurrence selected by adjacent tied windows is one
                    // persisted event. Canonicalize before spill so qualification,
                    // authority receipt, and the encoder all count the same
                    // `(hash, encoded_position)` stream. The external merge still
                    // deduplicates across arbitrary runs as a defensive invariant.
                    buffers.run.erase(std::ranges::unique(buffers.run).begin(), buffers.run.end());
                    receipt.peak_core_events =
                        std::max<std::uint64_t>(receipt.peak_core_events, buffers.run.size());
                    if (!buffers.run.empty()) {
                        const direct_minimizer_spill_storage_binding before_spill = storage_binding;
                        storage.spill_sorted_segment_run(
                            static_cast<std::uint32_t>(segment_ordinal),
                            std::span<const detail::sorted_pair_t>{buffers.run});
                        storage_binding = storage.segment_spill_storage_binding();
                        if (!storage_binding.valid() ||
                            storage_binding.generation_token != before_spill.generation_token ||
                            before_spill.mutation_revision ==
                                (std::numeric_limits<std::uint64_t>::max)() ||
                            storage_binding.mutation_revision !=
                                before_spill.mutation_revision + 1u) {
                            return std::unexpected(
                                minimizer_generation_encoding_error::replay_mismatch);
                        }
                        minimizer_sequence_generation_execution_receipt::add(
                            receipt.spill_run_count, 1u, receipt.saturated);
                        minimizer_sequence_generation_execution_receipt::add(
                            receipt.spilled_event_count, buffers.run.size(), receipt.saturated);
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
                storage, target_length, position_segment_span_bp, minimizer_scheme};
        const auto qualified = qualification_producer.scan_sorted_events(
            position_segment_span_bp,
            canonical_minimizer_event_receiver{
                .context = &qualification,
                /** @brief Counts one qualified canonical event during spill sealing. */
                .emit_fn = +[](void* context, const canonical_minimizer_event& event)
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
            return std::unexpected(minimizer_generation_encoding_error::duplicate_event);
        }
        const minimizer_sequence_generation_execution_receipt qualification_receipt =
            qualification_producer.execution_receipt();
        receipt.spill_content_digest = qualification_receipt.replay_first_completed_content_digest;
        receipt.spill_qualification_scan_count = qualification_producer.scan_count();
        receipt.spill_qualification_merge_count = qualification_producer.merge_pass_count();
        const std::uint64_t operation_epoch =
            minimizer_sequence_generation_execution_receipt::next_spill_operation_epoch();
        const std::uint64_t authority_seal =
            minimizer_sequence_generation_execution_receipt::next_spill_authority_seal();
        if (operation_epoch == 0 || authority_seal == 0) {
            return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
        }
        receipt.spill_operation_epoch = operation_epoch;
        receipt.spill_storage_binding = storage_binding;
        auto authority = std::shared_ptr<const direct_minimizer_spill_authority<p_storage>>{
            new direct_minimizer_spill_authority<p_storage>{storage,
                                                            target_length,
                                                            position_segment_span_bp,
                                                            receipt.spill_run_count,
                                                            receipt.spilled_event_count,
                                                            receipt.spill_content_digest,
                                                            storage_binding,
                                                            operation_epoch,
                                                            authority_seal,
                                                            minimizer_scheme}};
        receipt.bind_spill_authority(authority_seal,
                                     receipt.spilled_event_count,
                                     receipt.spill_content_digest,
                                     storage_binding,
                                     operation_epoch);
        receipt.spill_complete = true;
        receipt.capture_route_snapshot_();
        if (!receipt.is_spill_valid()) {
            return std::unexpected(minimizer_generation_encoding_error::storage_contract_mismatch);
        }
        return generation_spill_result{receipt.spill_run_count,
                                       receipt.exact_simd_stats,
                                       receipt.exact_simd_stats_available,
                                       minimizer_scheme,
                                       target_length,
                                       position_segment_span_bp,
                                       receipt,
                                       std::move(authority)};
    }

    /**
     * @brief Reopen a qualified spill for deterministic replay.
     */
    [[nodiscard]] static inline auto make_generation_replay_producer(
        p_storage& storage,
        std::uint64_t target_length,
        generation_spill_result&& spill,
        std::uint64_t position_segment_span_bp = k_minimizer_preferred_segment_span_bp) {
        static_assert(direct_minimizer_qualified_spill_storage<p_storage>);
        minimizer_sequence_generation_execution_receipt receipt = spill.execution_receipt_;
        std::shared_ptr<const direct_minimizer_spill_authority<p_storage>> authority =
            std::move(spill.authority_);
        const bool spill_identity_matches =
            authority != nullptr && authority->storage_ == &storage &&
            authority->target_length_ == target_length &&
            authority->initial_span_bp_ == position_segment_span_bp &&
            authority->run_count_ == spill.run_count_ &&
            authority->event_count_ == receipt.spilled_event_count &&
            authority->content_digest_ == receipt.spill_content_digest &&
            authority->storage_binding_ == receipt.spill_storage_binding &&
            storage.segment_spill_storage_binding() == authority->storage_binding_ &&
            authority->operation_epoch_ == receipt.spill_operation_epoch &&
            authority->authority_seal_ != 0 &&
            authority->minimizer_scheme_ == spill.minimizer_scheme_ &&
            spill.target_length_ == target_length &&
            spill.position_segment_span_bp_ == position_segment_span_bp &&
            receipt.authority_matches(authority->authority_seal_,
                                      authority->event_count_,
                                      authority->content_digest_,
                                      authority->storage_binding_,
                                      authority->operation_epoch_);
        const bool spill_identity_is_consistent =
            spill_identity_matches && spill.minimizer_scheme_ == receipt.minimizer_scheme &&
            spill.run_count_ == receipt.spill_run_count &&
            spill.exact_simd_stats_available_ == receipt.exact_simd_stats_available;
        if (!spill_identity_is_consistent) {
            receipt.backend_bound = false;
            authority.reset();
        }
        const minimizer_scheme_identity producer_scheme =
            spill_identity_is_consistent ? spill.minimizer_scheme_ : minimizer_scheme_identity{};
        spill.poison_();
        return direct_minimizer_external_spill_replay_producer<p_storage>{
            storage,
            target_length,
            position_segment_span_bp,
            producer_scheme,
            receipt,
            std::move(authority),
            receipt.spill_storage_binding};
    }

    /// Encode grouped-SoA rows from a sealed direct minimizer-bucket spill. The layout
    /// consumes the sealed direct minimizer-bucket spill without conversion.

    /**
     * @brief Encode grouped-SoA rows from a qualified direct-minimizer spill.
     */
    template <minimizer_generation_row_sink p_sink>
    [[nodiscard]] static inline std::expected<encoded_grouped_soa_generation,
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
        if (options.layout != minimizer_generation_layout::grouped_soa_segment_local ||
            options.window_size != 10 || options.kmer_size != 15 ||
            spill.position_segment_span_bp_ != k_minimizer_preferred_segment_span_bp ||
            spill.authority_ == nullptr || spill.authority_->storage_ != &storage ||
            storage.segment_spill_storage_binding() != spill.authority_->storage_binding_ ||
            !spill.execution_receipt_.is_spill_valid()) {
            return std::unexpected(minimizer_generation_encoding_error::storage_contract_mismatch);
        }
        auto producer = make_generation_replay_producer(storage,
                                                        options.target_length,
                                                        std::move(spill),
                                                        k_minimizer_preferred_segment_span_bp);
        auto encoded = encode_minimizer_generation(producer, sink, options, limits);
        minimizer_sequence_generation_execution_receipt receipt = producer.execution_receipt();
        if (encoded) {
            receipt.manifest_position_count = encoded->manifest.position_count;
            receipt.manifest_position_segment_span_bp = encoded->manifest.position_segment_span_bp;
            receipt.manifest_bound =
                encoded->manifest.layout == options.layout &&
                encoded->manifest.layout == receipt.generation_layout &&
                encoded->manifest.target_length == receipt.target_length &&
                encoded->manifest.position_count == receipt.spilled_event_count &&
                encoded->manifest.kmer_size == receipt.kmer_size &&
                encoded->manifest.window_size == receipt.window_size &&
                encoded->manifest.minimizer_scheme_id == receipt.minimizer_scheme.id &&
                encoded->manifest.minimizer_scheme_revision == receipt.minimizer_scheme.revision &&
                encoded->manifest.producer_backend_id ==
                    minimizer_producer_backend_identity_for_backend(receipt.selected_backend).id &&
                encoded->manifest.producer_identity_revision ==
                    k_minimizer_producer_identity_revision;
            if (receipt.manifest_bound) {
                receipt.bind_manifest_authority();
                receipt.bind_publication_authority(*encoded);
                receipt.capture_replay_snapshot_();
            }
            if (!receipt.is_internally_consistent()) {
                encoded =
                    std::unexpected(minimizer_generation_encoding_error::storage_contract_mismatch);
            }
        }
        if (execution_receipt_out != nullptr) {
            *execution_receipt_out = receipt;
        }
        return encoded;
    }
};
} // namespace bioseq::indexing
