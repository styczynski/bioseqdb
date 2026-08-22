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

export import bioseq.indexing.minimizer_sequence.generation;
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

template <class p_storage>
struct direct_minimizer_bucket_index : direct_minimizer_generation_replay<p_storage> {
    static_assert(is_index_chunked_storage_for<p_storage, sequence_index_layout::fragment_chunks>);
    static_assert(is_external_sort_spill_storage<p_storage>);
    static constexpr sequence_index_layout index_type = sequence_index_layout::fragment_chunks;
    static constexpr std::string_view index_name = k_minimizers_per_sequence_index_name;
    using storage_type = p_storage;

    using generation_base = direct_minimizer_generation_replay<p_storage>;
    using typename generation_base::generation_spill_result;
    using generation_base::collect_generation_core_run;
    using generation_base::encode_persisted_generation;
    using generation_base::make_generation_collection_range;
    using generation_base::make_generation_replay_producer;
    using generation_base::spill_generation_replay_runs_serial;

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
