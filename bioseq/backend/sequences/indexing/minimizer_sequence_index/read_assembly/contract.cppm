/**
 * @file minimizer_sequence_index/read_assembly/contract.cppm
 * @brief Public direct minimizer index contract and spill authority ownership.
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

export module bioseq.indexing.minimizer_sequence:read_assembly_contract;

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

template <class p_storage> struct direct_minimizer_bucket_index {
    static_assert(is_index_chunked_storage_for<p_storage, sequence_index_layout::fragment_chunks>);
    static_assert(is_external_sort_spill_storage<p_storage>);
    static constexpr sequence_index_layout index_type = sequence_index_layout::fragment_chunks;
    static constexpr std::string_view index_name = k_minimizers_per_sequence_index_name;
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
            : run_count_(run_count), exact_simd_stats_(exact_simd_stats),
              exact_simd_stats_available_(exact_simd_stats_available),
              minimizer_scheme_(minimizer_scheme), target_length_(target_length),
              position_segment_span_bp_(position_segment_span_bp),
              execution_receipt_(execution_receipt), authority_(std::move(authority)) {}

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
        [[nodiscard]] const exact_simd_index_execution_stats& exact_simd_stats() const noexcept {
            return exact_simd_stats_;
        }
        [[nodiscard]] bool exact_simd_stats_available() const noexcept {
            return exact_simd_stats_available_;
        }
        [[nodiscard]] minimizer_scheme_identity minimizer_scheme() const noexcept {
            return minimizer_scheme_;
        }
        [[nodiscard]] std::uint64_t target_length() const noexcept {
            return target_length_;
        }
        [[nodiscard]] std::uint64_t position_segment_span_bp() const noexcept {
            return position_segment_span_bp_;
        }
        [[nodiscard]] const minimizer_sequence_generation_execution_receipt&
        execution_receipt() const noexcept {
            return execution_receipt_;
        }
    };

    template <sequence_alphabet p_alphabet>
    [[nodiscard]] static inline std::expected<segment_unit_range,
                                              minimizer_generation_encoding_error>
    make_generation_collection_range(bioseq::storage::lazy_sequence_view<p_alphabet> sequence,
                                     std::uint64_t core_lo,
                                     std::uint64_t core_hi,
                                     std::uint64_t halo_bp = 10u + 15u - 2u) noexcept;

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
        direct_minimizer_bucket_build_stats* scalar_build_stats = nullptr);

    /// Prepare external runs serially within the bounded production budget. Each source core is
    /// collected and spilled before the next is allocated. The returned direct minimizer-bucket
    /// producer reopens these same external runs for both encoder passes.
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
        std::uint8_t alphabet_code = bioseq::minimizers::k_unbound_packed_alphabet_code);

    [[nodiscard]] static inline auto make_generation_replay_producer(
        p_storage& storage,
        std::uint64_t target_length,
        generation_spill_result&& spill,
        std::uint64_t position_segment_span_bp = k_minimizer_preferred_segment_span_bp);

    /// Encode grouped-SoA rows from a sealed direct minimizer-bucket spill. The layout
    /// consumes the sealed direct minimizer-bucket spill without conversion.
    template <minimizer_generation_row_sink p_sink>
    [[nodiscard]] static inline std::expected<encoded_grouped_soa_generation,
                                              minimizer_generation_encoding_error>
    encode_persisted_generation(
        p_storage& storage,
        p_sink& sink,
        generation_spill_result&& spill,
        const grouped_soa_generation_options& options,
        const grouped_soa_generation_limits& limits = {},
        minimizer_sequence_generation_execution_receipt* execution_receipt_out = nullptr);

    template <sequence_alphabet p_alphabet>
    static inline void
    build(p_storage& storage,
          bioseq::storage::lazy_sequence_view<p_alphabet> seq,
          minimizer_sketch_backend backend,
          const bioseq::minimizers::minimizer_kernels* kernels,
          direct_minimizer_bucket_build_stats* scalar_build_stats_out = nullptr,
          std::uint8_t alphabet_code = bioseq::minimizers::k_unbound_packed_alphabet_code);

    template <sequence_alphabet p_alphabet>
    static inline void build(p_storage& storage,
                             bioseq::storage::lazy_sequence_view<p_alphabet> seq,
                             minimizer_sketch_backend backend,
                             const bioseq::minimizers::minimizer_kernels* kernels,
                             const std::uint8_t alphabet_code);

    // scalar-default overload delegates to the explicit backend.
    template <sequence_alphabet p_alphabet>
    static inline void build(p_storage& storage,
                             bioseq::storage::lazy_sequence_view<p_alphabet> seq);

    template <sequence_alphabet p_alphabet>
    static inline void
    rebuild(p_storage& storage,
            bioseq::storage::lazy_sequence_view<p_alphabet> seq,
            minimizer_sketch_backend backend,
            const bioseq::minimizers::minimizer_kernels* kernels,
            std::uint8_t alphabet_code = bioseq::minimizers::k_unbound_packed_alphabet_code);

    template <sequence_alphabet p_alphabet>
    static inline void rebuild(p_storage& storage,
                               bioseq::storage::lazy_sequence_view<p_alphabet> seq);

    /// query-time: load ONLY the buckets containing any query kmer (§10.5).
    static inline minimizer_hashmap_fragment_index
    assemble_for_query(p_storage& storage,
                       std::span<const minimap_sketch_data> query_minimizers,
                       sequence_size_t target_len,
                       std::int32_t max_occ,
                       minimizer_assembly_scratch& scratch);

    /// query-time: load query-touched buckets but keep only positions in one
    /// target collection range. Occurrence filtering remains global.
    static inline minimizer_hashmap_fragment_index
    assemble_for_query_segment(p_storage& storage,
                               std::span<const minimap_sketch_data> query_minimizers,
                               sequence_size_t target_len,
                               std::int32_t max_occ,
                               sequence_size_t clip_lo_bp,
                               sequence_size_t clip_hi_bp,
                               minimizer_assembly_scratch& scratch);
};

} // namespace bioseq::indexing
export namespace bioseq::indexing {
template <> struct index_descriptor<direct_minimizer_bucket_index> {
    static constexpr sequence_index_layout index_type = sequence_index_layout::fragment_chunks;
    static constexpr std::string_view index_name = k_minimizers_per_sequence_index_name;
    static constexpr bool allows_update_on_insert =
        true; // opt-in external sort; not a fresh-table default
};
static_assert(
    requires { index_descriptor<direct_minimizer_bucket_index>::index_name; },
    "direct_minimizer_bucket_index index_descriptor must be exported");
} // namespace bioseq::indexing
