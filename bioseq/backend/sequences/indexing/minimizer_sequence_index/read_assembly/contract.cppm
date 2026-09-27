/**
 * @file minimizer_sequence_index/read_assembly/contract.cppm
 * @brief Public direct minimizer index contract and spill authority ownership.
 */
module;

#include <cstdint>
#include <expected>
#include <memory>
#include <new>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

export module bioseq.indexing.minimizer_sequence:read_assembly_contract;

export import bioseq.indexing.minimizer_sequence.generation;
import bioseq.indexing.minimizer_sequence.admitted_runs;
import bioseq.indexing.minimizer_sequence.bucket_codec;
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
import bioseq.sequences.kmers.highway_minimizer_contract;
import bioseq.sequences.kmers.vulkan_minimizers;
import bioseq.storage.lazy_sequence_view;
import bioseq.storage.size;

export namespace bioseq::indexing {

using bioseq::minimizers::minimap_sketch_data;
using bioseq::minimizers::minimizer_sketch_backend;
using bioseq::minimizers::minimizer_sketch_backend_name;
using bioseq::minimizers::sketch_collector;
using bioseq::simd::collect_minimizers_view;
using bioseq::storage::sequence_size_t;

template <class p_storage> struct direct_minimizer_bucket_index {
    static_assert(is_index_chunked_storage_for<p_storage, sequence_index_layout::fragment_chunks>);
    static_assert(is_external_sort_spill_storage<p_storage>);
    static constexpr sequence_index_layout index_type = sequence_index_layout::fragment_chunks;
    static constexpr std::string_view index_name = k_minimizers_per_sequence_index_name;
    using storage_type = p_storage;

    /// Share the sealed producer handle; the facade must not duplicate its receipt fields.
    using generation_spill_result = generation_detail::generation_spill_result<
        p_storage, direct_minimizer_bucket_index<p_storage>>;

    template <sequence_alphabet p_alphabet>
    [[nodiscard]] static std::expected<segment_unit_range,
                                       minimizer_generation_encoding_error>
    make_generation_collection_range(bioseq::storage::lazy_sequence_view<p_alphabet> sequence,
                                     std::uint64_t core_lo,
                                     std::uint64_t core_hi,
                                     std::uint16_t window_size,
                                     std::uint16_t kmer_size) noexcept {
        return direct_minimizer_generation_replay<
            p_storage,
            direct_minimizer_bucket_index<p_storage>>::make_generation_collection_range(
            sequence, core_lo, core_hi, window_size, kmer_size);
    }

    template <sequence_alphabet p_alphabet>
    [[nodiscard]] static std::expected<exact_simd_index_execution_stats,
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
        return direct_minimizer_generation_replay<
            p_storage,
            direct_minimizer_bucket_index<p_storage>>::collect_generation_core_run(
            sequence,
            alphabet_code,
            backend,
            kernels,
            range,
            window_size,
            kmer_size,
            algorithm_identity,
            collector,
            output,
            execution_receipt,
            allow_packed_bounded,
            scalar_build_stats);
    }

    /// Prepare external runs serially within the bounded production budget. Each source core is
    /// collected and spilled before the next is allocated. The returned direct minimizer-bucket
    /// producer reopens these same external runs for both encoder passes.
    template <sequence_alphabet p_alphabet>
    [[nodiscard]] static std::expected<generation_spill_result,
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
        return direct_minimizer_generation_replay<
            p_storage,
            direct_minimizer_bucket_index<p_storage>>::spill_generation_replay_runs_serial(
            storage,
            sequence,
            backend,
            kernels,
            algorithm_identity,
            core_span_bp,
            position_segment_span_bp,
            execution_receipt_out,
            alphabet_code,
            selected_hashes_u32,
            selected_hashes_u64,
            selected_hash_width);
    }

    [[nodiscard]] static auto make_generation_replay_producer(
        p_storage& storage,
        std::uint64_t target_length,
        generation_spill_result&& spill,
        std::uint64_t position_segment_span_bp = k_minimizer_preferred_segment_span_bp) {
        return direct_minimizer_generation_replay<
            p_storage,
            direct_minimizer_bucket_index<p_storage>>::make_generation_replay_producer(
            storage, target_length, std::move(spill), position_segment_span_bp);
    }

    /// Encode grouped-SoA rows from a sealed direct minimizer-bucket spill. The layout
    /// consumes the sealed direct minimizer-bucket spill without conversion.
    template <minimizer_generation_row_sink p_sink>
    [[nodiscard]] static std::expected<encoded_grouped_soa_generation,
                                       minimizer_generation_encoding_error>
    encode_persisted_generation(
        p_storage& storage,
        p_sink& sink,
        generation_spill_result&& spill,
        const grouped_soa_generation_options& options,
        const grouped_soa_generation_limits& limits = {},
        minimizer_sequence_generation_execution_receipt* execution_receipt_out = nullptr) {
        return direct_minimizer_generation_replay<
            p_storage,
            direct_minimizer_bucket_index<p_storage>>::encode_persisted_generation(
            storage, sink, std::move(spill), options, limits, execution_receipt_out);
    }

    template <sequence_alphabet p_alphabet>
    static void
    build(p_storage& storage,
          bioseq::storage::lazy_sequence_view<p_alphabet> seq,
          minimizer_sketch_backend backend,
          const bioseq::minimizers::minimizer_kernels* kernels,
          direct_minimizer_bucket_build_stats* scalar_build_stats_out = nullptr,
          std::uint8_t alphabet_code = bioseq::minimizers::k_unbound_packed_alphabet_code);

    template <sequence_alphabet p_alphabet>
    static void build(p_storage& storage,
                      bioseq::storage::lazy_sequence_view<p_alphabet> seq,
                      minimizer_sketch_backend backend,
                      const bioseq::minimizers::minimizer_kernels* kernels,
                      const std::uint8_t alphabet_code);

    // scalar-default overload delegates to the explicit backend.
    template <sequence_alphabet p_alphabet>
    static void build(p_storage& storage,
                      bioseq::storage::lazy_sequence_view<p_alphabet> seq);

    template <sequence_alphabet p_alphabet>
    static void
    rebuild(p_storage& storage,
            bioseq::storage::lazy_sequence_view<p_alphabet> seq,
            minimizer_sketch_backend backend,
            const bioseq::minimizers::minimizer_kernels* kernels,
            std::uint8_t alphabet_code = bioseq::minimizers::k_unbound_packed_alphabet_code);

    template <sequence_alphabet p_alphabet>
    static void rebuild(p_storage& storage,
                        bioseq::storage::lazy_sequence_view<p_alphabet> seq);

    /// query-time: load ONLY the buckets containing any query kmer (§10.5).
    static minimizer_hashmap_fragment_index
    assemble_for_query(p_storage& storage,
                       std::span<const minimap_sketch_data> query_minimizers,
                       sequence_size_t target_len,
                       std::int32_t max_occ,
                       minimizer_assembly_scratch& scratch);

    /// query-time: load query-touched buckets but keep only positions in one
    /// target collection range. Occurrence filtering remains global.
    static minimizer_hashmap_fragment_index
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
