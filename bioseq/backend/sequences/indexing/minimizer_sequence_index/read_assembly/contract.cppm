/**
 * @file minimizer_sequence_index/read_assembly/contract.cppm
 * @brief Declares direct minimizer index building and query lookup assembly.
 */
module;

#include <algorithm>
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
import bioseq.indexing.hash_major_minimizer_index;
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

    /// Use the generation module's checked spill handle and execution recort
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
            nullptr,
            execution_receipt,
            allow_packed_bounded,
            scalar_build_stats);
    }

    /// Collect minimizers into checked spill runs within the memory budget.
    /// The encoder replays these runs for planning and writing the index.
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

    /// Consume a checked spill handle to write grouped-SoA index rows.
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

    // Use the scalar backend when, none is specified.
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

    /// Load buckets selected by the query hashes and assemble their matching positions into a lookup.
    static minimizer_hashmap_fragment_index
    assemble_for_query(p_storage& storage,
                       std::span<const minimap_sketch_data> query_minimizers,
                       sequence_size_t target_len,
                       std::int32_t max_occ,
                       minimizer_assembly_scratch& scratch);

    /// Assemble  matching positions within [clip_lo_bp, clip_hi_bp).
    /// Apply the occurrence limit to each hash's full target count before clipping.
    static minimizer_hashmap_fragment_index
    assemble_for_query_segment(p_storage& storage,
                               std::span<const minimap_sketch_data> query_minimizers,
                               sequence_size_t target_len,
                               std::int32_t max_occ,
                               sequence_size_t clip_lo_bp,
                               sequence_size_t clip_hi_bp,
                               minimizer_assembly_scratch& scratch);

    /// Assemble native-width query hashes using the same directory and bucket reader.
    static minimizer_hashmap_fragment_index assemble_for_query_segment(
        p_storage& storage, canonical_query_hash_view query_hashes,
        sequence_size_t target_len, std::int32_t max_occ,
        sequence_size_t clip_lo_bp, sequence_size_t clip_hi_bp,
        minimizer_assembly_scratch& scratch) {
        if (!query_hashes.storage_matches_width()) {
            storage.index_error("minimizers_per_sequence: invalid canonical query hash view");
        }
        scratch.clear_query();
        scratch.query_kmers.reserve(query_hashes.size());
        for (std::size_t ordinal = 0u; ordinal < query_hashes.size(); ++ordinal) {
            const std::uint64_t hash = query_hashes.at(ordinal);
            if (ordinal != 0u && query_hashes.at(ordinal - 1u) >= hash) {
                storage.index_error("ordinary minimizer query hashes must be sorted and unique");
            }
            scratch.query_kmers.push_back(hash);
        }
        return assemble_for_query_hashes_(storage, target_len, max_occ, clip_lo_bp, clip_hi_bp, scratch);
    }

  private:
    /// Read buckets after the caller supplies query hashes in scratch.
    static minimizer_hashmap_fragment_index assemble_for_query_hashes_(
        p_storage& storage, sequence_size_t target_len, std::int32_t max_occ,
        sequence_size_t clip_lo_bp, sequence_size_t clip_hi_bp,
        minimizer_assembly_scratch& scratch) {
        using detail::k_encoded_header_bytes;
        using detail::k_encoded_bucket_bytes;
        clip_hi_bp = std::min(clip_hi_bp, target_len);
        if (clip_lo_bp > clip_hi_bp) {
            clip_lo_bp = clip_hi_bp;
        }
        // Copy the directory so later  bucket reads can reuse the storage buffer.
        const char* dblob = storage.template read_chunk<char>(k_directory_sub_key, 0);
        const std::size_t dbytes = storage.chunk_byte_size(k_directory_sub_key, 0);
        storage.validate_minimizer_directory_or_error(dblob, dbytes);
        direct_minimizer_bucket_directory_view dir{dblob, dbytes};
        const std::uint32_t max_occ_threshold =
            detail::effective_max_occ_threshold(max_occ);
        // Collect unique query hashes and the bucket keys whose ranges contain thm.
        auto& qkmers = scratch.query_kmers;
        auto& wanted = scratch.wanted_sub_keys;
        for (const std::uint64_t hash : qkmers) {
            if (const direct_minimizer_bucket_directory_entry* entry = dir.find(hash))
                wanted.push_back(entry->sub_key);
        }
        auto sort_unique = [](auto& v) {
            std::sort(v.begin(), v.end());
            v.erase(std::unique(v.begin(), v.end()), v.end());
        };
        sort_unique(wanted);
        // Read the selected buckets and discard hashes above the occurrence limit.
        auto& pairs = scratch.pairs;
        const auto pair_bound = detail::minimizer_sequence_query_pair_bound(
            clip_hi_bp - clip_lo_bp, static_cast<std::uint64_t>(qkmers.size()), max_occ);
        const auto accept_bucket = [&qkmers, &storage, target_len, max_occ_threshold](
                                       const std::uint64_t hash, const std::uint32_t count) {
            if (count > max_occ_threshold ||
                !std::binary_search(qkmers.begin(), qkmers.end(), hash)) {
                return false;
            }
            // Validate before wire_for_each_bucket resizes position scratch.
            if (count > target_len) {
                storage.index_error("minimizers_per_sequence: bucket count exceeds target length");
            }
            return true;
        };
        auto emit_kept = [&pairs, &storage, clip_lo_bp, clip_hi_bp, pair_bound](
                             std::uint64_t kmer, std::uint32_t pos_count, const std::uint64_t* gpos) {
            for (std::uint32_t j = 0; j < pos_count; ++j) {
                const sequence_size_t pos_bp = static_cast<sequence_size_t>(gpos[j] >> 1);
                if (pos_bp < clip_lo_bp || pos_bp >= clip_hi_bp) {
                    continue;
                }
                // Malformed repeated bucket descriptors must not exceed admission.
                if (pairs.size() >= pair_bound) {
                    storage.index_error("minimizers_per_sequence: query postings exceed admission bound");
                }
                pairs.emplace_back(kmer, gpos[j]);
            }
        };
        for (std::int64_t sk : wanted) {
            const std::size_t nc = storage.number_of_chunks(sk);
            if (nc == 0)
                storage.index_error(
                    "minimizers_per_sequence: directory references a bucket with no chunks");
            if (nc > 1) {
                // A multi-chunk bucket contains one hash Count its positions from chunk
                // sizes before deciding whether to load the payloads.
                std::uint64_t exact = 0u;
                for (std::size_t c = 0; c < nc; ++c) {
                    storage.validate_minimizer_chunk_metadata_or_error(
                        sk, c, k_encoded_header_bytes + k_encoded_bucket_bytes, alignof(std::uint64_t));
                    const std::size_t cb = storage.chunk_byte_size(sk, c);
                    exact = bioseq::common::saturating_add(exact, static_cast<std::uint64_t>(
                        (cb - k_encoded_header_bytes - k_encoded_bucket_bytes) / sizeof(std::uint64_t)));
                }
                if (exact > max_occ_threshold)
                    continue;                          // The full count exceeds the occurrence limit.
                for (std::size_t c = 0; c < nc; ++c) { // Read every  chunk of this hash.
                    const std::size_t cb = storage.chunk_byte_size(sk, c);
                    const char* p = storage.template read_chunk<char>(sk, c);
                    const auto h = storage.validate_minimizer_blob_or_error(p, cb, 1);
                    if (h.bucket_count != 1)
                        storage.index_error(
                            "assemble_for_query: multi-chunk bucket with bucket_count!=1 (byte-math invalid)");
                    wire_for_each_bucket(p, cb, scratch.bucket_positions, accept_bucket, emit_kept);
                }
                continue;
            }
            const std::size_t cb0 = storage.chunk_byte_size(sk, 0);
            const char* p0 = storage.template read_chunk<char>(sk, 0);
            storage.validate_minimizer_blob_or_error(p0, cb0);
            wire_for_each_bucket(p0, cb0, scratch.bucket_positions, accept_bucket, emit_kept);
        }
        // Build the query's in-memory lookup from the selected hash/position pairs.
        minimizer_hashmap_fragment_index idx;
        idx.build_from_pairs(pairs, target_len);
        return idx;
    }
};

} // namespace bioseq::indexing
export namespace bioseq::indexing {
template <> struct index_descriptor<direct_minimizer_bucket_index> {
    static constexpr sequence_index_layout index_type = sequence_index_layout::fragment_chunks;
    static constexpr std::string_view index_name = k_minimizers_per_sequence_index_name;
    static constexpr bool allows_update_on_insert =
        true; // Supports insertion updates when this index is explicitly selected.
};
static_assert(
    requires { index_descriptor<direct_minimizer_bucket_index>::index_name; },
    "direct_minimizer_bucket_index index_descriptor must be exported");
} // namespace bioseq::indexing
