/**
 * @file minimizer_sequence_index/generation/operations.cppm
 * @brief Collects minimizers into checked spill runs and replays them to write grouped indexes.
 */
module;

#include <new>

export module bioseq.indexing.minimizer_sequence.generation:operations;

import std;
import bioseq.pipeline.runtime_contract;
import :replay_contract;
import bioseq.indexing.minimizer_sequence.admitted_runs;
import bioseq.indexing.minimizer_sequence.bucket_codec;
import bioseq.common.checked_arithmetic;
import bioseq.gpu.vulkan.abi;
import bioseq.gpu.vulkan.runtime;
import bioseq.indexing.minimizer_hashmap;
import bioseq.indexing.minimizer_index;
import bioseq.indexing.storage;
import bioseq.indexing.type;
import bioseq.parallelism.memory_admission;
import bioseq.sequences;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.highway_minimizer_contract;
import bioseq.sequences.kmers.minimizer_physical_registry;
import bioseq.sequences.kmers.vulkan_minimizers;
import bioseq.storage.lazy_sequence;
import bioseq.storage.lazy_sequence_view;
import bioseq.storage.size;

export namespace bioseq::indexing {

using bioseq::minimizers::minimap_sketch_data;
using bioseq::minimizers::minimizer_sketch_backend;
using bioseq::minimizers::sketch_collector;
using bioseq::storage::sequence_size_t;

namespace generation_detail {

/** Move-only handle for spill runs checked against their storage identity and event digest. */
template <class p_storage, class p_result_owner> class generation_spill_result {
      private:
        friend struct direct_minimizer_generation_replay<p_storage, p_result_owner>;

        std::uint64_t run_count_ = 0;
        exact_simd_index_execution_stats exact_simd_stats_{};
        bool exact_simd_stats_available_ = false;
        reference_simd_index_execution_stats reference_simd_stats_{};
        bool reference_simd_stats_available_ = false;
        minimizer_scheme_identity minimizer_scheme_{};
        std::uint64_t target_length_ = 0;
        std::uint64_t position_segment_span_bp_ = 0;
        minimizer_sequence_generation_execution_receipt execution_receipt_{};
        std::shared_ptr<const direct_minimizer_spill_authority<p_storage>> authority_{};

        /** Store the counts, algorithm settings and checked identity of a completed spill. */
        generation_spill_result(
            std::uint64_t run_count,
            const exact_simd_index_execution_stats& exact_simd_stats,
            bool exact_simd_stats_available,
            const reference_simd_index_execution_stats& reference_simd_stats,
            bool reference_simd_stats_available,
            minimizer_scheme_identity minimizer_scheme,
            std::uint64_t target_length,
            std::uint64_t position_segment_span_bp,
            const minimizer_sequence_generation_execution_receipt& execution_receipt,
            std::shared_ptr<const direct_minimizer_spill_authority<p_storage>> authority) noexcept
            : run_count_(run_count), exact_simd_stats_(exact_simd_stats),
              exact_simd_stats_available_(exact_simd_stats_available),
              reference_simd_stats_(reference_simd_stats),
              reference_simd_stats_available_(reference_simd_stats_available),
              minimizer_scheme_(minimizer_scheme), target_length_(target_length),
              position_segment_span_bp_(position_segment_span_bp),
              execution_receipt_(execution_receipt), authority_(std::move(authority)) {}

        /** Clear this handle so it cannot be used for another replay. */
        void poison_() noexcept {
            run_count_ = 0;
            exact_simd_stats_ = {};
            exact_simd_stats_available_ = false;
            reference_simd_stats_ = {};
            reference_simd_stats_available_ = false;
            minimizer_scheme_ = {};
            target_length_ = 0;
            position_segment_span_bp_ = 0;
            execution_receipt_ = {};
            authority_.reset();
        }

        /** Transfer the spill result and clear the source handle. */
        void steal_from_(generation_spill_result&& other) noexcept {
            run_count_ = other.run_count_;
            exact_simd_stats_ = other.exact_simd_stats_;
            exact_simd_stats_available_ = other.exact_simd_stats_available_;
            reference_simd_stats_ = other.reference_simd_stats_;
            reference_simd_stats_available_ = other.reference_simd_stats_available_;
            minimizer_scheme_ = other.minimizer_scheme_;
            target_length_ = other.target_length_;
            position_segment_span_bp_ = other.position_segment_span_bp_;
            execution_receipt_ = std::move(other.execution_receipt_);
            authority_ = std::move(other.authority_);
            other.poison_();
        }

      public:
        /** A spill handle can only be created after its runs have been checked. */
        generation_spill_result() = delete;
        /** Copying is disabled so the handle can be consumed only once. */
        generation_spill_result(const generation_spill_result&) = delete;
        /** Copy assignment is disabled so the handle can be consumed only once. */
        generation_spill_result& operator=(const generation_spill_result&) = delete;
        /** Transfer the spill handle and clear the source. */
        generation_spill_result(generation_spill_result&& other) noexcept {
            steal_from_(std::move(other));
        }
        /** Release this handle, then take the source handle and clear it. */
        generation_spill_result& operator=(generation_spill_result&& other) noexcept {
            if (this != &other) {
                poison_();
                steal_from_(std::move(other));
            }
            return *this;
        }

        /** Return the number of runs stored by this spill operation. */
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
        /// Return minimap2-reference execution stats accumulated during spill.
        [[nodiscard]] const reference_simd_index_execution_stats&
        reference_simd_stats() const noexcept {
            return reference_simd_stats_;
        }
        /// Return whether minimap2-reference stats were recorded during spill.
        [[nodiscard]] bool reference_simd_stats_available() const noexcept {
            return reference_simd_stats_available_;
        }
        /** Return the hash and minimizer selection scheme used by the spill. */
        [[nodiscard]] minimizer_scheme_identity minimizer_scheme() const noexcept {
            return minimizer_scheme_;
        }
        /** @brief Returns the target sequence length covered by the spill. */
        [[nodiscard]] std::uint64_t target_length() const noexcept {
            return target_length_;
        }
        /** Return the position-segment size, in bases, used when the runs were spilled. */
        [[nodiscard]] std::uint64_t position_segment_span_bp() const noexcept {
            return position_segment_span_bp_;
        }
        /** Borrow the checked execution record for this spill operation. */
        [[nodiscard]] const minimizer_sequence_generation_execution_receipt&
        execution_receipt() const noexcept {
            return execution_receipt_;
        }
};

template <class p_storage, class p_result_owner> struct generation_spill_result_for {
    using type = typename p_result_owner::generation_spill_result;
};

template <class p_storage> struct generation_spill_result_for<p_storage, void> {
    using type = generation_spill_result<p_storage, void>;
};

} // namespace generation_detail

/**
 * @brief Builds spill runs and replays them for index generation.
 * Persistence code can use these operations without importing query assembly.
 * @tparam p_storage Storage with fragment chunks and external sorting support.
 */
template <class p_storage, class p_result_owner> struct direct_minimizer_generation_replay {
    static_assert(is_index_chunked_storage_for<p_storage, sequence_index_layout::fragment_chunks>);
    static_assert(is_external_sort_spill_storage<p_storage>);

    using generation_spill_result =
        typename generation_detail::generation_spill_result_for<p_storage, p_result_owner>::type;

    /**
     * Expand a core by the k-mer/window overlap needed on each side, clipping at
     * the sequence ends. Only positions inside the original core may be emitted.
     */
    template <sequence_alphabet p_alphabet>
    [[nodiscard]] static inline std::expected<segment_unit_range,
                                              minimizer_generation_encoding_error>
    make_generation_collection_range(bioseq::storage::lazy_sequence_view<p_alphabet> sequence,
                                     std::uint64_t core_lo,
                                     std::uint64_t core_hi,
                                     std::uint16_t window_size,
                                     std::uint16_t kmer_size) noexcept {
        const std::uint64_t target_length = sequence.bp_size();
        const std::uint64_t shape_span =
            static_cast<std::uint64_t>(window_size) + static_cast<std::uint64_t>(kmer_size);
        if (core_lo > core_hi || core_hi > target_length || shape_span < 2u) {
            return std::unexpected(minimizer_generation_encoding_error::bp_range);
        }
        const std::uint64_t halo_bp = shape_span - 2u;
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
     * Collect minimizers for one core using the selected backend and algorithm.
     * Read its surrounding overlap, then append global hash/position pairs only
     * for positions inside the core. Record execution statistics when requested.
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
        std::uint16_t window_size,
        std::uint16_t kmer_size,
        const minimizer_generation_algorithm_identity* algorithm_identity,
        sketch_collector& collector,
        std::vector<detail::sorted_pair_t>& output,
        minimizer_sequence_generation_execution_receipt* execution_receipt = nullptr,
        bool allow_packed_bounded = false,
        direct_minimizer_bucket_build_stats* scalar_build_stats = nullptr) {
        const std::uint64_t target_length = sequence.bp_size();
        const std::uint64_t shape_span =
            static_cast<std::uint64_t>(window_size) + static_cast<std::uint64_t>(kmer_size);
        const bool kernel_binding_valid =
            algorithm_identity != nullptr
                ? minimizer_generation_kernel_binding_is_valid(
                      backend, *algorithm_identity, kernels)
                : !((backend == minimizer_sketch_backend::exact_simd &&
                     (kernels == nullptr || kernels->two_stack_nthash == nullptr)) ||
                    (backend == minimizer_sketch_backend::bounded_nthash_simd &&
                     (kernels == nullptr || kernels->bounded_nthash == nullptr)));
        if (shape_span < 2u ||
            (algorithm_identity != nullptr &&
             (algorithm_identity->runtime_kmer_size != kmer_size ||
              algorithm_identity->runtime_window_size != window_size ||
              algorithm_identity->alphabet_id !=
                  bioseq::minimizers::minimizer_profile_alphabet_id<p_alphabet>() ||
              !is_valid_minimizer_generation_algorithm_identity(
                  *algorithm_identity,
                  minimizer_producer_backend_identity_for_backend(backend)))) ||
            !kernel_binding_valid) {
            return std::unexpected(minimizer_generation_encoding_error::storage_contract_mismatch);
        }
        if (range.core_lo_bp > range.core_hi_bp || range.core_hi_bp > target_length ||
            range.cs_u > range.ce_u || range.ce_u > target_length) {
            return std::unexpected(minimizer_generation_encoding_error::bp_range);
        }
        if (range.core_lo_bp == range.core_hi_bp || range.cs_u == range.ce_u) {
            return exact_simd_index_execution_stats{};
        }
        const auto expected_range =
            make_generation_collection_range(sequence,
                                             range.core_lo_bp,
                                             range.core_hi_bp,
                                             window_size,
                                             kmer_size);
        if (!expected_range || expected_range->cs_u != range.cs_u ||
            expected_range->ce_u != range.ce_u ||
            expected_range->core_lo_bp != range.core_lo_bp ||
            expected_range->core_hi_bp != range.core_hi_bp) {
            return std::unexpected(minimizer_generation_encoding_error::bp_range);
        }
        const std::uint64_t emit_base = range.cs_u;
        const std::uint64_t halo_bp = shape_span - 2u;
        const std::uint64_t collection_hi =
            range.core_hi_bp > target_length - std::min(target_length, halo_bp)
                ? target_length
                : range.core_hi_bp + halo_bp;
        const std::uint64_t local_bp_size =
            collection_hi > emit_base ? collection_hi - emit_base : 0u;
        const std::uint64_t minimum_work_bases = shape_span - 1u;
        /** Add to an optional backend counter, recording overflow. */
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
                    window_size,
                    kmer_size,
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
        if (backend == minimizer_sketch_backend::scalar && algorithm_identity != nullptr) {
            const auto selected_layout =
                select_minimizer_generation_physical_layout<p_alphabet>(
                    algorithm_identity->requested_algorithm,
                    target_length,
                    algorithm_identity->packed_storage_bits,
                    kmer_size,
                    window_size);
            if (!selected_layout) {
                return std::unexpected(
                    minimizer_generation_encoding_error::storage_contract_mismatch);
            }
            const auto selected_identity =
                make_minimizer_generation_algorithm_identity(*selected_layout);
            if (!selected_identity || *selected_identity != *algorithm_identity) {
                return std::unexpected(
                    minimizer_generation_encoding_error::storage_contract_mismatch);
            }
            add_route(&minimizer_sequence_generation_execution_receipt::scalar_core_runs);
            try {
                (void)bioseq::minimizers::visit_minimizer_physical_layout(
                    *selected_layout,
                    [&sequence,
                     range,
                     emit_base,
                     window_size,
                     kmer_size,
                     &output]<class p_minimizer>() {
                        struct canonical_pair_collector {
                            std::vector<detail::sorted_pair_t>* output = nullptr;
                            std::uint64_t emit_base = 0u;
                            std::uint64_t core_lo = 0u;
                            std::uint64_t core_hi = 0u;

                            /** Append a scalar minimizer if its global position lies inside the core. */
                            void collect(const p_minimizer& minimizer) {
                                const std::uint64_t position =
                                    emit_base +
                                    static_cast<std::uint64_t>(minimizer.last_pos());
                                if (position < core_lo || position >= core_hi) {
                                    return;
                                }
                                output->emplace_back(
                                    minimizer.kmer(),
                                    (position << 1u) |
                                        static_cast<std::uint64_t>(minimizer.strand()));
                            }
                        } direct_collector{
                            .output = &output,
                            .emit_base = emit_base,
                            .core_lo = range.core_lo_bp,
                            .core_hi = range.core_hi_bp,
                        };
                        bioseq::simd::collect_minimizers_segment_scalar<p_minimizer>(
                            direct_collector,
                            sequence,
                            range.cs_u,
                            range.ce_u,
                            window_size,
                            kmer_size);
                    });
            } catch (const std::invalid_argument&) {
                return std::unexpected(
                    minimizer_generation_encoding_error::storage_contract_mismatch);
            }
            if (scalar_build_stats != nullptr) {
                scalar_build_stats->record_scalar_collection(
                    range.ce_u - range.cs_u, range.ce_u - range.cs_u, 1u, 1u);
            }
            return exact_simd_stats;
        }
        if (backend == minimizer_sketch_backend::exact_simd) {
            add_route(&minimizer_sequence_generation_execution_receipt::exact_simd_core_runs);
            if (algorithm_identity != nullptr &&
                algorithm_identity->effective_algorithm ==
                    bioseq::simd::minimizer_algorithm::simd_minimap2_reference) {
                const auto selected_layout =
                    select_minimizer_generation_physical_layout<p_alphabet>(
                        algorithm_identity->requested_algorithm,
                        target_length,
                        algorithm_identity->packed_storage_bits,
                        kmer_size,
                        window_size);
                if (!selected_layout) {
                    return std::unexpected(
                        minimizer_generation_encoding_error::storage_contract_mismatch);
                }
                const auto selected_identity =
                    make_minimizer_generation_algorithm_identity(*selected_layout);
                if (!selected_identity || *selected_identity != *algorithm_identity) {
                    return std::unexpected(
                        minimizer_generation_encoding_error::storage_contract_mismatch);
                }
                try {
                    const auto reference_stats =
                        bioseq::minimizers::visit_minimizer_physical_layout(
                            *selected_layout,
                            [&sequence,
                             alphabet_code,
                             range,
                             emit_base,
                             window_size,
                             kmer_size,
                             algorithm_identity,
                             kernels,
                             &output]<class p_minimizer>() {
                                struct canonical_pair_collector {
                                    std::vector<detail::sorted_pair_t>* output = nullptr;
                                    std::uint64_t emit_base = 0u;
                                    std::uint64_t core_lo = 0u;
                                    std::uint64_t core_hi = 0u;

                                    /// Append a reference-SIMD minimizer if its global position lies inside the core.
                                    void collect(const p_minimizer& minimizer) {
                                        const std::uint64_t position =
                                            emit_base +
                                            static_cast<std::uint64_t>(minimizer.last_pos());
                                        if (position < core_lo || position >= core_hi) {
                                            return;
                                        }
                                        output->emplace_back(
                                            minimizer.kmer(),
                                            (position << 1u) |
                                                static_cast<std::uint64_t>(minimizer.strand()));
                                    }
                                } direct_collector{
                                    .output = &output,
                                    .emit_base = emit_base,
                                    .core_lo = range.core_lo_bp,
                                    .core_hi = range.core_hi_bp,
                                };
                                return collect_reference_simd_index_range_or_throw<p_minimizer>(
                                    direct_collector,
                                    sequence,
                                    alphabet_code,
                                    range.cs_u,
                                    range.ce_u,
                                    window_size,
                                    kmer_size,
                                    algorithm_identity->packed_storage_bits,
                                    kernels,
                                    "minimizers_per_sequence build");
                            });
                    if (execution_receipt != nullptr) {
                        execution_receipt->reference_simd_stats.merge(reference_stats);
                        execution_receipt->reference_simd_stats_available = true;
                        execution_receipt->saturated =
                            execution_receipt->saturated ||
                            execution_receipt->reference_simd_stats.saturated;
                    }
                } catch (const exact_simd_index_runtime_error&) {
                    return std::unexpected(
                        minimizer_generation_encoding_error::storage_contract_mismatch);
                } catch (const std::invalid_argument&) {
                    return std::unexpected(
                        minimizer_generation_encoding_error::storage_contract_mismatch);
                }
            } else {
                exact_simd_stats =
                    collect_exact_simd_index_range_or_throw(collector,
                                                            sequence,
                                                            alphabet_code,
                                                            range.cs_u,
                                                            range.ce_u,
                                                            window_size,
                                                            kmer_size,
                                                            0u,
                                                            kernels,
                                                            "minimizers_per_sequence build");
                if (execution_receipt != nullptr) {
                    execution_receipt->exact_simd_stats.merge(exact_simd_stats);
                    execution_receipt->exact_simd_stats_available = true;
                    execution_receipt->saturated = execution_receipt->saturated ||
                                                   execution_receipt->exact_simd_stats.saturated;
                }
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
                                                         window_size,
                                                         kmer_size,
                                                         kernels,
                                                         collector,
                                                         &materialized_window_bp,
                                                         algorithm_identity != nullptr
                                                             ? algorithm_identity->packed_storage_bits
                                                             : 0u);
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
                collector, sequence, range.cs_u, range.ce_u, window_size, kmer_size);
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

    /// Spill completed core runs while keeping the collection buffers within their memory permit.
    /// The index encoder rereads these runs for planning and writing its payloads.

    /**
     * Collect, sort and deduplicate minimizer runs, optionally keeping only query
     * hashes. Store each completed run, then scan the merged events to record
     * their count and SHA-256 digest before returning a checked spill handle.
     */
    template <sequence_alphabet p_alphabet>
    [[nodiscard]] static inline std::expected<generation_spill_result,
                                              minimizer_generation_encoding_error>
    spill_generation_replay_runs_serial(
        p_storage& storage,
        bioseq::storage::lazy_sequence_view<p_alphabet> sequence,
        minimizer_sketch_backend backend,
        const bioseq::minimizers::minimizer_kernels* kernels,
        minimizer_generation_algorithm_identity algorithm_identity,
        std::uint64_t core_span_bp = 0,
        std::uint64_t position_segment_span_bp = k_minimizer_preferred_segment_span_bp,
        minimizer_sequence_generation_execution_receipt* execution_receipt_out = nullptr,
        std::uint8_t alphabet_code = bioseq::minimizers::k_unbound_packed_alphabet_code,
        std::span<const std::uint32_t> selected_hashes_u32 = {},
        std::span<const std::uint64_t> selected_hashes_u64 = {},
        std::uint8_t selected_hash_width = 0u) {
        static_assert(direct_minimizer_qualified_spill_storage<p_storage>);
        constexpr std::uint64_t default_core_span = 32'768u;
        constexpr std::uint64_t lpu = 1u;
        const std::uint64_t target_length = sequence.bp_size();
        struct receipt_output_guard {
            minimizer_sequence_generation_execution_receipt* output = nullptr;
            minimizer_sequence_generation_execution_receipt receipt{};
            /** Copy the accumulated execution record to the caller when this scope ends. */
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
        receipt.algorithm_identity = algorithm_identity;
        receipt.target_length = target_length;
        receipt.window_size = algorithm_identity.runtime_window_size;
        receipt.kmer_size = algorithm_identity.runtime_kmer_size;
        receipt.configured_core_span_bp = core_span_bp == 0 ? default_core_span : core_span_bp;
        const bool unfiltered = selected_hash_width == 0u && selected_hashes_u32.empty() &&
                                selected_hashes_u64.empty();
        const bool selected_u32 =
            selected_hash_width == sizeof(std::uint32_t) && selected_hashes_u64.empty() &&
            algorithm_identity.hash_width == bioseq::minimizers::minimizer_scalar_width::u32;
        const bool selected_u64 =
            selected_hash_width == sizeof(std::uint64_t) && selected_hashes_u32.empty() &&
            algorithm_identity.hash_width == bioseq::minimizers::minimizer_scalar_width::u64;
        const auto sorted_unique = []<class p_hash>(std::span<const p_hash> hashes) noexcept {
            return std::ranges::is_sorted(hashes) &&
                   std::adjacent_find(hashes.begin(), hashes.end()) == hashes.end();
        };
        if ((!unfiltered && !selected_u32 && !selected_u64) ||
            (selected_u32 && !sorted_unique(selected_hashes_u32)) ||
            (selected_u64 && !sorted_unique(selected_hashes_u64))) {
            return std::unexpected(minimizer_generation_encoding_error::storage_contract_mismatch);
        }
        const auto selected_identity =
            select_minimizer_generation_algorithm_identity<p_alphabet>(
                algorithm_identity.requested_algorithm,
                target_length,
                algorithm_identity.packed_storage_bits,
                algorithm_identity.runtime_kmer_size,
                algorithm_identity.runtime_window_size);
        const minimizer_producer_identity producer_backend =
            minimizer_producer_backend_identity_for_backend(backend);
        if (!selected_identity || *selected_identity != algorithm_identity ||
            !is_valid_minimizer_generation_algorithm_identity(algorithm_identity,
                                                              producer_backend) ||
            algorithm_identity.alphabet_id !=
                bioseq::minimizers::minimizer_profile_alphabet_id<p_alphabet>()) {
            return std::unexpected(minimizer_generation_encoding_error::storage_contract_mismatch);
        }
        if (!minimizer_generation_kernel_binding_is_valid(
                backend, algorithm_identity, kernels)) {
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
                // Free both collection buffers before releasing their memory reservation.
                bioseq::parallelism::memory_permit permit;
                std::vector<detail::sorted_pair_t> run;
                sketch_collector collector;
            } buffers;
            /** Reserve memory for collection buffers before growing them. */
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

            /** Sort a completed run, remove duplicates, apply the query filter and spill it. */
            const auto spill_completed_core =
                [selected_u32,
                 selected_u64,
                 selected_hashes_u32,
                 selected_hashes_u64,
                 &buffers,
                 &receipt,
                 &storage,
                 &storage_binding](std::uint64_t completed_segment_ordinal)
                -> std::expected<void, minimizer_generation_encoding_error> {
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
                // Adjacent windows can select the same hash and position. Store that pair
                // once so the spill counts and encoder agree. The external merge
                // also removes duplicates that occur in different runs.
                buffers.run.erase(std::ranges::unique(buffers.run).begin(), buffers.run.end());
                // Keep target hashes that can seed at least one query. Select the query
                // hash width once for this core, keeping 32-bit searches narrow.
                const auto retain_selected = [&buffers]<class p_hash>(
                                                 std::span<const p_hash> selected) {
                    buffers.run.erase(
                        std::remove_if(
                            buffers.run.begin(),
                            buffers.run.end(),
                            [selected](const detail::sorted_pair_t& event) noexcept {
                                if constexpr (sizeof(p_hash) == sizeof(std::uint32_t)) {
                                    if (event.first >
                                        (std::numeric_limits<std::uint32_t>::max)()) {
                                        return true;
                                    }
                                }
                                return !std::binary_search(selected.begin(),
                                                           selected.end(),
                                                           static_cast<p_hash>(event.first));
                            }),
                        buffers.run.end());
                };
                if (selected_u32) {
                    retain_selected(selected_hashes_u32);
                } else if (selected_u64) {
                    retain_selected(selected_hashes_u64);
                }
                receipt.peak_core_events =
                    std::max<std::uint64_t>(receipt.peak_core_events, buffers.run.size());
                if (buffers.run.empty()) {
                    return {};
                }
                const direct_minimizer_spill_storage_binding before_spill = storage_binding;
                storage.spill_sorted_segment_run(
                    static_cast<std::uint32_t>(completed_segment_ordinal),
                    std::span<const detail::sorted_pair_t>{buffers.run});
                storage_binding = storage.segment_spill_storage_binding();
                if (!storage_binding.valid() ||
                    storage_binding.generation_token != before_spill.generation_token ||
                    before_spill.mutation_revision ==
                        (std::numeric_limits<std::uint64_t>::max)() ||
                    storage_binding.mutation_revision != before_spill.mutation_revision + 1u) {
                    return std::unexpected(minimizer_generation_encoding_error::replay_mismatch);
                }
                minimizer_sequence_generation_execution_receipt::add(
                    receipt.spill_run_count, 1u, receipt.saturated);
                minimizer_sequence_generation_execution_receipt::add(
                    receipt.spilled_event_count, buffers.run.size(), receipt.saturated);
                if (receipt.saturated) {
                    return std::unexpected(
                        minimizer_generation_encoding_error::arithmetic_overflow);
                }
                return {};
            };

            /** Collect one core, then sort and spill its minimizers. */
            const auto collect_and_spill_core =
                [sequence,
                 alphabet_code,
                 backend,
                 kernels,
                 lpu,
                 &algorithm_identity,
                 &buffers,
                 &receipt,
                 &ensure_replay_capacity,
                 &spill_completed_core](std::uint64_t completed_segment_ordinal,
                                        std::uint64_t core_lo,
                                        std::uint64_t core_hi)
                -> std::expected<void, minimizer_generation_encoding_error> {
                auto range = make_generation_collection_range(sequence,
                                                              core_lo,
                                                              core_hi,
                                                              receipt.window_size,
                                                              receipt.kmer_size);
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
                // Bounded ntHash can emit a selected minimum and up to a window of tied
                // records for each candidate. Reserve that raw output before
                // collection. Sorting and deduplication reduce it before storage.
                // The other CPU paths reserve at most one record per input base.
                const std::uint64_t maximum_bounded_records_per_candidate =
                    static_cast<std::uint64_t>(receipt.window_size) + 1u;
                const bool direct_canonical_output =
                    backend == minimizer_sketch_backend::scalar ||
                    (backend == minimizer_sketch_backend::exact_simd &&
                     algorithm_identity.effective_algorithm ==
                         bioseq::simd::minimizer_algorithm::simd_minimap2_reference);
                const std::uint64_t expected_sketches =
                    backend == minimizer_sketch_backend::vulkan_segment ||
                            direct_canonical_output
                        ? 0u
                        : (backend == minimizer_sketch_backend::bounded_nthash_simd
                               ? bioseq::common::saturating_mul(
                                     maximum_new_sketches,
                                     maximum_bounded_records_per_candidate)
                               : maximum_new_sketches);
                const std::uint64_t expected_run =
                    backend == minimizer_sketch_backend::vulkan_segment
                        ? core_hi - core_lo
                        : (direct_canonical_output ? maximum_new_sketches
                                                   : expected_sketches);
                if (!ensure_replay_capacity(expected_run, expected_sketches)) {
                    return std::unexpected(minimizer_generation_encoding_error::admission);
                }
                auto collected = collect_generation_core_run(sequence,
                                                             alphabet_code,
                                                             backend,
                                                             kernels,
                                                             *range,
                                                             receipt.window_size,
                                                             receipt.kmer_size,
                                                             &algorithm_identity,
                                                             buffers.collector,
                                                             buffers.run,
                                                             &receipt,
                                                             true);
                if (!collected) {
                    return std::unexpected(collected.error());
                }
                return spill_completed_core(completed_segment_ordinal);
            };

            const std::uint64_t shape_span =
                static_cast<std::uint64_t>(receipt.window_size) + receipt.kmer_size;
            const bool batch_vulkan =
                backend == minimizer_sketch_backend::vulkan_segment && shape_span >= 2u &&
                target_length >= shape_span - 1u;
            if (batch_vulkan) {
                const std::uint64_t halo_bp = shape_span - 2u;
                const std::uint64_t maximum_upload_window_bp = std::min(
                    target_length,
                    bioseq::common::saturating_add(
                        core_span_bp, bioseq::common::saturating_mul(halo_bp, 2u)));
                auto batched_spill = bioseq::gpu::vulkan::
                    with_vulkan_segment_pair_record<p_alphabet>(
                        receipt.window_size,
                        receipt.kmer_size,
                        maximum_upload_window_bp,
                        [sequence,
                         core_span_bp,
                         position_segment_span_bp,
                         target_length,
                         &buffers,
                         &receipt,
                         &segment_lo,
                         &segment_ordinal,
                         &ensure_replay_capacity,
                         &collect_and_spill_core,
                         &spill_completed_core]<class p_minimizer_type>()
                            -> std::expected<void, minimizer_generation_encoding_error> {
                            using canonical_window = bioseq::gpu::vulkan::
                                admitted_sequence_detail::canonical_u32_window<p_alphabet>;
                            using canonical_window_batch = bioseq::gpu::vulkan::
                                admitted_sequence_detail::canonical_u32_window_batch<p_alphabet>;
                            using upload_instance = bioseq::gpu::vulkan::
                                admitted_sequence_detail::borrowed_u32_instance<p_alphabet>;
                            using upload_range = compact_sequence_range<upload_instance>;

                            /** Collection and emission ranges for one Vulkan upload. */
                            struct vulkan_replay_core_geometry {
                                segment_unit_range range{};
                            };

                            const bioseq::gpu::vulkan::vulkan_rank_lut rank_lut =
                                bioseq::gpu::vulkan::make_vulkan_rank_lut<upload_range>();
                            while (segment_lo < target_length) {
                                if (!sql_integer_can_represent(segment_ordinal)) {
                                    return std::unexpected(
                                        minimizer_generation_encoding_error::segment_count);
                                }
                                const std::uint64_t segment_hi =
                                    segment_lo + std::min(position_segment_span_bp,
                                                          target_length - segment_lo);
                                std::optional<canonical_window> pending_owner;
                                std::optional<vulkan_replay_core_geometry> pending_geometry;
                                std::uint64_t core_lo = segment_lo;
                                while (core_lo < segment_hi) {
                                    buffers.run.clear();
                                    auto plan = bioseq::gpu::vulkan::
                                        make_minimap_sketch_data_vulkan_upload_batch_admission_plan<
                                            p_minimizer_type>(
                                                rank_lut,
                                                receipt.window_size,
                                                receipt.kmer_size,
                                                bioseq::gpu::vulkan::
                                                    vulkan_sketch_host_materialization::
                                                        host_and_device);
                                    if (!plan) {
                                        pending_owner.reset();
                                        pending_geometry.reset();
                                        const std::uint64_t core_hi =
                                            core_lo + std::min(core_span_bp,
                                                               segment_hi - core_lo);
                                        auto single = collect_and_spill_core(
                                            segment_ordinal, core_lo, core_hi);
                                        if (!single) {
                                            return std::unexpected(single.error());
                                        }
                                        core_lo = core_hi;
                                        continue;
                                    }

                                    // Keep source buffers alive until the synchronous batch finishes.
                                    // Destruction frees their borrowed descriptors first.
                                    canonical_window_batch source_owners;
                                    bioseq::gpu::vulkan::sequence_upload_batch upload_batch;
                                    std::vector<vulkan_replay_core_geometry> geometries;
                                    std::uint64_t candidate_core_lo = core_lo;
                                    while (candidate_core_lo < segment_hi) {
                                        vulkan_replay_core_geometry geometry{};
                                        std::optional<canonical_window> candidate_owner;
                                        if (pending_owner) {
                                            if (!pending_geometry ||
                                                pending_geometry->range.core_lo_bp !=
                                                    candidate_core_lo) {
                                                throw bioseq::gpu::vulkan::vulkan_runtime_error{
                                                    "Vulkan spill batch retained invalid core "
                                                    "geometry"};
                                            }
                                            geometry = *pending_geometry;
                                            candidate_owner.emplace(std::move(*pending_owner));
                                            pending_owner.reset();
                                            pending_geometry.reset();
                                        } else {
                                            const std::uint64_t candidate_core_hi =
                                                candidate_core_lo +
                                                std::min(core_span_bp,
                                                         segment_hi - candidate_core_lo);
                                            auto range = make_generation_collection_range(
                                                sequence,
                                                candidate_core_lo,
                                                candidate_core_hi,
                                                receipt.window_size,
                                                receipt.kmer_size);
                                            if (!range) {
                                                return std::unexpected(range.error());
                                            }
                                            receipt.peak_core_span_bp = std::max(
                                                receipt.peak_core_span_bp,
                                                candidate_core_hi - candidate_core_lo);
                                            if (!ensure_replay_capacity(
                                                    candidate_core_hi - candidate_core_lo, 0u)) {
                                                return std::unexpected(
                                                    minimizer_generation_encoding_error::
                                                        admission);
                                            }
                                            geometry.range = *range;
                                            bioseq::gpu::vulkan::vulkan_sketch_decline decline{
                                                .bp_size = static_cast<std::uint64_t>(
                                                    range->ce_u - range->cs_u),
                                                .window_size = receipt.window_size,
                                                .kmer_size = receipt.kmer_size,
                                            };
                                            auto window = bioseq::gpu::vulkan::
                                                admitted_sequence_detail::
                                                    prepare_canonical_u32_window(
                                                        sequence,
                                                        range->cs_u,
                                                        range->ce_u,
                                                        &decline);
                                            if (!window) {
                                                break;
                                            }
                                            candidate_owner.emplace(std::move(*window));
                                        }

                                        auto upload = bioseq::gpu::vulkan::
                                            admitted_sequence_detail::
                                                with_canonical_u32_upload_source<p_alphabet>(
                                                    *candidate_owner,
                                                    []<class p_range>(p_range& resident) {
                                                        return bioseq::gpu::vulkan::
                                                            make_sequence_upload_view(resident);
                                                    });
                                        if (!bioseq::gpu::vulkan::
                                                try_append_minimap_sketch_data_vulkan_upload(
                                                    *plan, upload)) {
                                            pending_owner.emplace(
                                                std::move(*candidate_owner));
                                            pending_geometry = geometry;
                                            break;
                                        }
                                        source_owners.push_back(
                                            std::move(*candidate_owner));
                                        upload_batch.push_back(std::move(upload));
                                        geometries.push_back(geometry);
                                        if (geometry.range.core_hi_bp <= candidate_core_lo) {
                                            return std::unexpected(
                                                minimizer_generation_encoding_error::
                                                    arithmetic_overflow);
                                        }
                                        candidate_core_lo = geometry.range.core_hi_bp;
                                    }

                                    if (geometries.empty()) {
                                        pending_owner.reset();
                                        pending_geometry.reset();
                                        const std::uint64_t core_hi =
                                            core_lo + std::min(core_span_bp,
                                                               segment_hi - core_lo);
                                        auto single = collect_and_spill_core(
                                            segment_ordinal, core_lo, core_hi);
                                        if (!single) {
                                            return std::unexpected(single.error());
                                        }
                                        core_lo = core_hi;
                                        continue;
                                    }

                                    const std::size_t accepted_count =
                                        plan->accepted_upload_count();
                                    if (accepted_count != geometries.size() ||
                                        accepted_count != source_owners.size() ||
                                        accepted_count != upload_batch.size() ||
                                        !plan->bind_accepted_uploads(upload_batch.view())) {
                                        throw bioseq::gpu::vulkan::vulkan_runtime_error{
                                            "Vulkan spill batch admission did not bind its "
                                            "exact prefix"};
                                    }
                                    std::uint64_t accepted_prefix_hi = core_lo;
                                    for (const auto& geometry : geometries) {
                                        if (geometry.range.core_lo_bp != accepted_prefix_hi ||
                                            geometry.range.core_hi_bp <=
                                                geometry.range.core_lo_bp ||
                                            geometry.range.core_hi_bp > segment_hi) {
                                            throw bioseq::gpu::vulkan::vulkan_runtime_error{
                                                "Vulkan spill batch accepted a noncontiguous or "
                                                "invalid core prefix"};
                                        }
                                        const std::uint64_t geometry_core_span =
                                            geometry.range.core_hi_bp -
                                            geometry.range.core_lo_bp;
                                        if (geometry_core_span > buffers.run.capacity()) {
                                            throw bioseq::gpu::vulkan::vulkan_runtime_error{
                                                "Vulkan spill batch core exceeded its admitted "
                                                "replay capacity"};
                                        }
                                        accepted_prefix_hi = geometry.range.core_hi_bp;
                                    }
                                    if (accepted_prefix_hi != candidate_core_lo ||
                                        pending_owner.has_value() !=
                                            pending_geometry.has_value() ||
                                        (pending_geometry &&
                                         pending_geometry->range.core_lo_bp !=
                                             accepted_prefix_hi)) {
                                        throw bioseq::gpu::vulkan::vulkan_runtime_error{
                                            "Vulkan spill batch did not preserve its next core "
                                            "boundary"};
                                    }
                                    // This next core was excluded from the accepted batch. Free its buffer
                                    // before finalizing the batch reservation. The next iteration starts
                                    // at the same core boundary and prepares it again.
                                    pending_owner.reset();
                                    pending_geometry.reset();
                                    auto batch_result = bioseq::gpu::vulkan::
                                        execute_admitted_minimap_sketch_data_vulkan_upload_batch<
                                            p_minimizer_type>(std::move(*plan));

                                    // The synchronous call has finished. Release descriptors before their source buffers.
                                    upload_batch =
                                        bioseq::gpu::vulkan::sequence_upload_batch{};
                                    source_owners.clear();

                                    if (!batch_result) {
                                        // Retry each core through the Vulkan path. With no completed batch
                                        // result, only these per-core calls contribute execution counts.
                                        for (const auto& geometry : geometries) {
                                            auto single = collect_and_spill_core(
                                                segment_ordinal,
                                                geometry.range.core_lo_bp,
                                                geometry.range.core_hi_bp);
                                            if (!single) {
                                                return std::unexpected(single.error());
                                            }
                                        }
                                        core_lo = accepted_prefix_hi;
                                        continue;
                                    }
                                    if (batch_result->materialization !=
                                            bioseq::gpu::vulkan::
                                                vulkan_sketch_host_materialization::
                                                    host_and_device ||
                                        !batch_result->dispatch_stats.is_internally_consistent() ||
                                        batch_result->dispatch_stats.completed_roundtrips() != 1u ||
                                        batch_result->query_records.size() != accepted_count ||
                                        batch_result->device_minimizer_count !=
                                            batch_result->host_minimizers.size()) {
                                        throw bioseq::gpu::vulkan::vulkan_runtime_error{
                                            "Vulkan spill batch returned an invalid completed "
                                            "result"};
                                    }

                                    // Check every returned core and position before writing any spill run.
                                    // Copy events into the run buffer one core at a time afterward.
                                    std::uint64_t record_cursor = 0u;
                                    for (std::size_t core_index = 0u;
                                         core_index < accepted_count;
                                         ++core_index) {
                                        const auto& record =
                                            batch_result->query_records[core_index];
                                        const auto& geometry = geometries[core_index];
                                        const std::uint64_t expected_query_length =
                                            static_cast<std::uint64_t>(
                                                geometry.range.ce_u - geometry.range.cs_u);
                                        const std::uint64_t core_span =
                                            geometry.range.core_hi_bp -
                                            geometry.range.core_lo_bp;
                                        const std::uint64_t record_end =
                                            bioseq::common::saturating_add(
                                                static_cast<std::uint64_t>(
                                                    record.minimizer_offset),
                                                static_cast<std::uint64_t>(
                                                    record.minimizer_count));
                                        if (record.flags != 0u ||
                                            record.minimizer_offset != record_cursor ||
                                            record.query_len != expected_query_length ||
                                            record_end >
                                                batch_result->host_minimizers.size()) {
                                            throw bioseq::gpu::vulkan::vulkan_runtime_error{
                                                "Vulkan spill batch query partition is "
                                                "invalid"};
                                        }
                                        std::uint64_t clipped_core_minimizers = 0u;
                                        for (std::uint64_t minimizer_index =
                                                 record.minimizer_offset;
                                             minimizer_index < record_end;
                                             ++minimizer_index) {
                                            const p_minimizer_type& minimizer =
                                                batch_result->host_minimizers[
                                                    static_cast<std::size_t>(
                                                        minimizer_index)];
                                            if (minimizer.last_pos() >= record.query_len ||
                                                minimizer.last_pos() >
                                                    (std::numeric_limits<std::uint64_t>::max)() -
                                                        geometry.range.cs_u) {
                                                throw bioseq::gpu::vulkan::
                                                    vulkan_runtime_error{
                                                        "Vulkan spill batch minimizer position "
                                                        "is invalid"};
                                            }
                                            const std::uint64_t position =
                                                geometry.range.cs_u +
                                                static_cast<std::uint64_t>(
                                                    minimizer.last_pos());
                                            if (position >= geometry.range.core_lo_bp &&
                                                position < geometry.range.core_hi_bp) {
                                                if (clipped_core_minimizers >= core_span) {
                                                    throw bioseq::gpu::vulkan::
                                                        vulkan_runtime_error{
                                                            "Vulkan spill batch exceeded the "
                                                            "one-record-per-core-position bound"};
                                                }
                                                ++clipped_core_minimizers;
                                            }
                                        }
                                        record_cursor = record_end;
                                    }
                                    if (record_cursor !=
                                        batch_result->host_minimizers.size()) {
                                        throw bioseq::gpu::vulkan::vulkan_runtime_error{
                                            "Vulkan spill batch partitions do not cover host "
                                            "output"};
                                    }

                                    bioseq::gpu::vulkan::
                                        vulkan_segment_pair_execution_stats batch_stats{};
                                    batch_stats.record_completed_roundtrip(
                                        batch_result->host_minimizers.size());
                                    receipt.vulkan_stats.merge(batch_stats);
                                    receipt.saturated =
                                        receipt.saturated || receipt.vulkan_stats.saturated;
                                    // One GPU round trip can process several cores.
                                    minimizer_sequence_generation_execution_receipt::add(
                                        receipt.vulkan_core_runs,
                                        static_cast<std::uint64_t>(accepted_count),
                                        receipt.saturated);
                                    for (std::size_t core_index = 0u;
                                         core_index < accepted_count;
                                         ++core_index) {
                                        const auto& record =
                                            batch_result->query_records[core_index];
                                        const auto& geometry = geometries[core_index];
                                        buffers.run.clear();
                                        const std::uint64_t core_span =
                                            geometry.range.core_hi_bp -
                                            geometry.range.core_lo_bp;
                                        const std::uint64_t record_end =
                                            static_cast<std::uint64_t>(
                                                record.minimizer_offset) +
                                            record.minimizer_count;
                                        for (std::uint64_t minimizer_index =
                                                 record.minimizer_offset;
                                             minimizer_index < record_end;
                                             ++minimizer_index) {
                                            const p_minimizer_type& minimizer =
                                                batch_result->host_minimizers[
                                                    static_cast<std::size_t>(
                                                        minimizer_index)];
                                            const std::uint64_t position =
                                                geometry.range.cs_u +
                                                static_cast<std::uint64_t>(
                                                    minimizer.last_pos());
                                            if (position < geometry.range.core_lo_bp ||
                                                position >= geometry.range.core_hi_bp) {
                                                continue;
                                            }
                                            if (buffers.run.size() >= core_span ||
                                                buffers.run.size() >=
                                                    buffers.run.capacity()) {
                                                throw bioseq::gpu::vulkan::
                                                    vulkan_runtime_error{
                                                        "Vulkan spill core append exceeded its "
                                                        "admitted core-span capacity"};
                                            }
                                            buffers.run.emplace_back(
                                                minimizer.kmer(),
                                                (position << 1u) |
                                                    static_cast<std::uint64_t>(
                                                        minimizer.strand()));
                                        }
                                        auto spilled =
                                            spill_completed_core(segment_ordinal);
                                        if (!spilled) {
                                            return std::unexpected(spilled.error());
                                        }
                                    }
                                    core_lo = accepted_prefix_hi;
                                }
                                segment_lo = segment_hi;
                                ++segment_ordinal;
                            }
                            return {};
                        });
                if (!batched_spill) {
                    return std::unexpected(batched_spill.error());
                }
            } else {
                while (segment_lo < target_length) {
                    if (!sql_integer_can_represent(segment_ordinal)) {
                        return std::unexpected(minimizer_generation_encoding_error::segment_count);
                    }
                    const std::uint64_t segment_hi =
                        segment_lo +
                        std::min(position_segment_span_bp, target_length - segment_lo);
                    for (std::uint64_t core_lo = segment_lo; core_lo < segment_hi;) {
                        const std::uint64_t core_hi =
                            core_lo + std::min(core_span_bp, segment_hi - core_lo);
                        auto spilled =
                            collect_and_spill_core(segment_ordinal, core_lo, core_hi);
                        if (!spilled) {
                            return std::unexpected(spilled.error());
                        }
                        core_lo = core_hi;
                    }
                    segment_lo = segment_hi;
                    ++segment_ordinal;
                }
            }
        } // Collection buffers are freed before their shared memory permit.

        // Scan the merged spill to record its event count and SHA-256 digest.
        // Replay checks these against the same storage generation. If a retry
        // halves the segment span, it checks the new event order and digest.
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
                /** Count each event in the merged spill, rejecting overflow. */
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
                                                            minimizer_scheme,
                                                            algorithm_identity}};
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
                                       receipt.reference_simd_stats,
                                       receipt.reference_simd_stats_available,
                                       minimizer_scheme,
                                       target_length,
                                       position_segment_span_bp,
                                       receipt,
                                       std::move(authority)};
    }

    /**
     * Consume a checked spill handle and reopen its runs for replay. Verify that
     * storage, algorithm, target length and segment size still match the handle.
     * A mismatch clears the checked spill identity.
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
            authority->algorithm_identity_ == receipt.algorithm_identity &&
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
            spill.exact_simd_stats_ == receipt.exact_simd_stats &&
            spill.exact_simd_stats_available_ == receipt.exact_simd_stats_available &&
            spill.reference_simd_stats_ == receipt.reference_simd_stats &&
            spill.reference_simd_stats_available_ == receipt.reference_simd_stats_available;
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

    /// Write grouped-SoA rows by replaying the checked spill runs.

    /**
     * Validate the spill and generation settings, then let the grouped encoder
     * replay the runs for planning and output. Check that the resulting manifest
     * describes the same events and algorithm, and return execution details when requested.
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
            options.kmer_size != options.algorithm_identity.runtime_kmer_size ||
            options.window_size != options.algorithm_identity.runtime_window_size ||
            spill.position_segment_span_bp_ != k_minimizer_preferred_segment_span_bp ||
            spill.authority_ == nullptr || spill.authority_->storage_ != &storage ||
            storage.segment_spill_storage_binding() != spill.authority_->storage_binding_ ||
            !spill.execution_receipt_.is_spill_valid()) {
            return std::unexpected(minimizer_generation_encoding_error::storage_contract_mismatch);
        }
        if (spill.execution_receipt_.algorithm_identity != options.algorithm_identity) {
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
                    k_minimizer_producer_identity_revision &&
                encoded->manifest.algorithm_identity == receipt.algorithm_identity;
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
