/**
 * @file minimizer_hashmap/fragment_producer.cppm
 * @brief Build fragment indexes, replay minimizers for encoding, and assemble query indexes.
 */
module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <libassert/assert.hpp>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

export module bioseq.indexing.minimizer_hashmap:fragment_producer;

import :exact_simd;
import bioseq.pipeline.runtime_contract;
import :grouped_soa;
import :prepared_reader;
import :compact_direct;
import :generation;
import bioseq.common.logging;
import bioseq.common.seqan_adapter;
import bioseq.gpu.vulkan.abi;
import bioseq.gpu.vulkan.runtime;
import bioseq.indexing.type;
import bioseq.indexing.storage;
import bioseq.indexing.traits;
import bioseq.indexing.minimizer_index;
import bioseq.indexing.compact_hash32_direct_index;
import bioseq.indexing.hash_major_minimizer_index;
import bioseq.common.checked_arithmetic;
import bioseq.sequences;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.highway_minimizer_contract;
import bioseq.sequences.kmers.minimizer_physical_registry;
import bioseq.sequences.kmers.vulkan_minimizers;
import bioseq.storage.scanner;
import bioseq.storage.lazy_sequence;
import bioseq.storage.size;
import bioseq.chaining.core;
import bioseq.common.radix_sort;
import bioseq.instrumentation;
import bioseq.parallelism.mode;
import bioseq.parallelism.pool;
import bioseq.parallelism.memory_admission;
import bioseq.parallelism.pipeline_ops;

export namespace bioseq::indexing {

/// Log unrecoverable index corruption and terminate the process.
inline void index_corruption_panic(std::string_view msg) {
    bioseq_log_progress<logging_level::error>("[bioseq index corruption] {}", msg);
    std::abort();
}

/// Caller-owned buffers reused when assembling indexes for query segments.
struct minimizer_assembly_scratch {
    /// Hashes paired  with global positions and strand bits for the assembled index.
    std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs;
    /// Sorted unique query k-mer hashes used to select stored matches.
    std::vector<std::uint64_t> query_kmers;
    /// Reusable position buffer passed to `wire_for_each_bucket`.
    std::vector<std::uint64_t> bucket_positions;
    /// Storage keys for buckets containing query hashes.
    std::vector<bioseq::indexing::index_sub_key_t> wanted_sub_keys;
    /// Clear assembled pairs while keeping the query hashes and buffer capacity.
    void clear_segment() noexcept {
        pairs.clear();
    }
    /// Clear every query-scoped scratch container without releasing capacity.
    void clear_query() noexcept {
        pairs.clear();
        query_kmers.clear();
        bucket_positions.clear();
        wanted_sub_keys.clear();
    }
};

// Walk serialized fragmnt indexes.

/// Visit every hash and encoded position in a serialized fragment index.
/// Check the header, byte count, and bucket ranges. Detected corruption calls
/// index_corruption_panic, which terminates the process.
/// @tparam p_function Callable accepting (hash, encoded_position).
template <class p_function>
void wire_for_each_entry(const char* blob, std::size_t blob_bytes, p_function&& emit) {
    using header = minimizer_hashmap_fragment_index::encoded_header;
    using bucket_record = minimizer_hashmap_fragment_index::encoded_bucket;
    if (blob_bytes < sizeof(header))
        index_corruption_panic("index chunk: truncated header");
    header decoded_header{};
    std::memcpy(&decoded_header, blob, sizeof(header));
    if (decoded_header.magic != minimizer_hashmap_fragment_index::k_encoded_magic ||
        decoded_header.layout_revision !=
            minimizer_hashmap_fragment_index::k_encoded_layout_revision)
        index_corruption_panic("index chunk: magic/layout");
    const std::size_t expect = sizeof(header) +
                               sizeof(bucket_record) * decoded_header.bucket_count +
                               sizeof(std::uint64_t) * decoded_header.total_positions;
    if (blob_bytes != expect)
        index_corruption_panic("index chunk: size mismatch (corruption)");
    const char* recs = blob + sizeof(header);
    const char* pos = recs + sizeof(bucket_record) * decoded_header.bucket_count;
    for (std::uint32_t bucket_no = 0; bucket_no < decoded_header.bucket_count; ++bucket_no) {
        bucket_record bucket{};
        std::memcpy(&bucket, recs + sizeof(bucket_record) * bucket_no, sizeof(bucket_record));
        if (static_cast<std::uint64_t>(bucket.pos_offset) + bucket.pos_count >
            decoded_header.total_positions)
            index_corruption_panic("index chunk: bucket range exceeds total_positions");
        for (std::uint32_t pos_no = 0; pos_no < bucket.pos_count; ++pos_no) {
            std::uint64_t encoded{};
            std::memcpy(&encoded,
                        pos + sizeof(std::uint64_t) * (bucket.pos_offset + pos_no),
                        sizeof(encoded));
            emit(bucket.hash, encoded);
        }
    }
}

/// Visit each complete hash bucket in a serialized fragment index.
/// Copy its positions into pos_scratch before  the callback. The pointer is
/// borrowed until the next bucket. Invalid headers, byte counts, or ranges
/// call index_corruption_panic and terminate the process.
/// @tparam p_function Callable accepting (hash, position_count, positions).
template <class p_function>
void wire_for_each_bucket(const char* blob,
                          std::size_t blob_bytes,
                          std::vector<std::uint64_t>& pos_scratch,
                          p_function&& cb) {
    using header = minimizer_hashmap_fragment_index::encoded_header;
    using bucket_record = minimizer_hashmap_fragment_index::encoded_bucket;
    if (blob_bytes < sizeof(header))
        index_corruption_panic("index chunk: truncated header");
    header decoded_header{};
    std::memcpy(&decoded_header, blob, sizeof(header));
    if (decoded_header.magic != minimizer_hashmap_fragment_index::k_encoded_magic ||
        decoded_header.layout_revision !=
            minimizer_hashmap_fragment_index::k_encoded_layout_revision)
        index_corruption_panic("index chunk: magic/layout");
    const std::size_t expect = sizeof(header) +
                               sizeof(bucket_record) * decoded_header.bucket_count +
                               sizeof(std::uint64_t) * decoded_header.total_positions;
    if (blob_bytes != expect)
        index_corruption_panic("index chunk: size mismatch (corruption)");
    const char* recs = blob + sizeof(header);
    const char* pos = recs + sizeof(bucket_record) * decoded_header.bucket_count;
    for (std::uint32_t bucket_no = 0; bucket_no < decoded_header.bucket_count; ++bucket_no) {
        bucket_record bucket{};
        std::memcpy(&bucket, recs + sizeof(bucket_record) * bucket_no, sizeof(bucket_record));
        if (static_cast<std::uint64_t>(bucket.pos_offset) + bucket.pos_count >
            decoded_header.total_positions)
            index_corruption_panic("index chunk: bucket range exceeds total_positions");
        pos_scratch.resize(bucket.pos_count);
        if (bucket.pos_count > 0)
            std::memcpy(pos_scratch.data(),
                        pos + sizeof(std::uint64_t) * bucket.pos_offset,
                        sizeof(std::uint64_t) * bucket.pos_count);
        cb(bucket.hash, bucket.pos_count, pos_scratch.data());
    }
}

/// Choose a GPU batch size from fragment count, estimated memory use, position
/// capacity, and dispatch limits. Return at least one, including for empty input
///
/// @param nfrag Total physical fragment count.
/// @param frag_bp Base pairs in one fragment core.
/// @param halo_bp Total context added around a batch core, in base pairs.
/// @param run_buffer_bytes Memory budget for one GPU submission.
[[nodiscard]] inline std::size_t
gpu_minimizer_fragments_per_batch(std::size_t nfrag,
                                  std::size_t frag_bp,
                                  std::size_t halo_bp,
                                  std::size_t run_buffer_bytes) noexcept {
    constexpr std::size_t max_bp =
        static_cast<std::size_t>(minimap_sketch_data::k_position_field_mask);
    if (nfrag == 0u || frag_bp == 0u) {
        return 1u;
    }
    constexpr std::size_t gpu_segment_local_size = 128u;
    constexpr std::size_t gpu_segment_max_workgroups = 65535u;
    constexpr std::size_t gpu_segment_bytes_per_bp = 96u;
    const std::size_t by_memory = std::max<std::size_t>(
        1u, run_buffer_bytes / std::max<std::size_t>(1u, frag_bp * gpu_segment_bytes_per_bp));
    const std::size_t by_pos =
        halo_bp >= max_bp ? 1u : std::max<std::size_t>(1u, (max_bp - halo_bp) / frag_bp);
    const std::size_t max_dispatch_bp = gpu_segment_local_size * gpu_segment_max_workgroups;
    const std::size_t by_dispatch =
        halo_bp >= max_dispatch_bp
            ? 1u
            : std::max<std::size_t>(1u, (max_dispatch_bp - halo_bp) / frag_bp);
    return std::min({nfrag, by_memory, by_pos, by_dispatch});
}

/// Serialize pairs already ordered by hash and encoded position  into a fragment blob.
/// Equal hashes become, one bucket. The kaller provides any required sorting
/// and duplicate removal.
///
/// @param target_len Fragment core length stored in the header.
/// @param out_blob Resized to hold the complete encoding.
inline void serialize_sorted_pairs_to_fragment_blob(
    std::span<const std::pair<std::uint64_t, std::uint64_t>> sorted_pairs,
    sequence_size_t target_len,
    std::vector<char>& out_blob) {
    using header = minimizer_hashmap_fragment_index::encoded_header;
    using bucket = minimizer_hashmap_fragment_index::encoded_bucket;

    std::vector<bucket> buckets;
    std::vector<std::uint64_t> positions;
    buckets.reserve(sorted_pairs.size() / 2u + 1u);
    positions.reserve(sorted_pairs.size());

    std::size_t index = 0;
    while (index < sorted_pairs.size()) {
        const std::uint64_t kmer = sorted_pairs[index].first;
        const std::uint32_t offset = static_cast<std::uint32_t>(positions.size());
        std::uint32_t count = 0;
        while (index < sorted_pairs.size() && sorted_pairs[index].first == kmer) {
            positions.push_back(sorted_pairs[index].second);
            ++count;
            ++index;
            if (count == std::numeric_limits<std::uint32_t>::max()) {
                index_corruption_panic("minimizers_per_fragment GPU blob: kmer count exceeds u32");
            }
        }
        buckets.push_back(bucket{kmer, offset, count});
    }

    const header blob_header{
        minimizer_hashmap_fragment_index::k_encoded_magic,
        minimizer_hashmap_fragment_index::k_encoded_layout_revision,
        static_cast<std::uint64_t>(target_len),
        static_cast<std::uint32_t>(buckets.size()),
        0u,
        static_cast<std::uint64_t>(positions.size()),
    };
    const std::size_t bytes =
        sizeof(header) + buckets.size() * sizeof(bucket) + positions.size() * sizeof(std::uint64_t);
    out_blob.resize(bytes);
    char* cursor = out_blob.data();
    std::memcpy(cursor, &blob_header, sizeof(blob_header));
    cursor += sizeof(header);
    if (!buckets.empty()) {
        std::memcpy(cursor, buckets.data(), buckets.size() * sizeof(bucket));
        cursor += buckets.size() * sizeof(bucket);
    }
    if (!positions.empty()) {
        std::memcpy(cursor, positions.data(), positions.size() * sizeof(std::uint64_t));
    }
}

/// Record which backend built the minimizers and how many producer scans ran.
/// Grouped-SoA encoding needs planning, control, and position scans. The SIMD
/// and Vulkan counters let publication checks compare the requested backend
/// with the work that completed.
class minimizer_fragment_generation_execution_receipt {
  public:
    minimizer_sketch_backend selected_backend = minimizer_sketch_backend::scalar;
    minimizer_scheme_identity minimizer_scheme{};
    minimizer_generation_algorithm_identity algorithm_identity{};
    std::uint64_t target_length = 0;
    std::uint16_t window_size = 0;
    std::uint16_t kmer_size = 0;
    /// Total producer scan attempts, including geometry retries.
    std::uint64_t producer_scan_count = 0;
    /// Extra scans recorded when `persisted_span` changes between replays.
    std::uint64_t geometry_retry_count = 0;
    minimizer_generation_layout generation_layout = minimizer_generation_layout::invalid;
    std::uint64_t short_core_runs = 0;
    std::uint64_t scalar_core_runs = 0;
    std::uint64_t exact_simd_core_runs = 0;
    std::uint64_t bounded_nthash_core_runs = 0;
    std::uint64_t vulkan_core_runs = 0;
    exact_simd_index_execution_stats exact_simd_stats{};
    bool exact_simd_stats_present = false;
    reference_simd_index_execution_stats reference_simd_stats{};
    bool reference_simd_stats_present = false;
    packed_bounded_nthash_index_status packed_bounded_status =
        packed_bounded_nthash_index_status::invalid_state;
    packed_bounded_nthash_index_execution_stats packed_bounded_stats{};
    bool packed_bounded_stats_present = false;
    bioseq::gpu::vulkan::vulkan_segment_pair_execution_stats vulkan_stats{};
    /// Whether the backend and parameter fields have been filled by the producer.
    bool backend_bound = false;
    /// Reject the receipt after  counter overflow or an invalid layout binding.
    bool saturated = false;

    /// Return the successful replay count required by the selected layout.
    [[nodiscard]] std::uint64_t required_successful_scan_count() const noexcept {
        if (generation_layout == minimizer_generation_layout::grouped_soa_segment_local) {
            return 3u;
        }
        /* Unbound producers can record two direct scans. Publication still
         * requires a supported storage layout matching the generation. */
        return generation_layout == minimizer_generation_layout::invalid ||
                       is_supported_minimizer_generation_layout(generation_layout)
                   ? 2u
                   : 0u;
    }

    /// Check backend identity and counters, allowing extra planning scans for geometry retries.
    [[nodiscard]] bool is_accepted() const noexcept {
        // A geometry retry adds one planning scan. The storage layout
        // determines how many successful scans complete the encoding.
        const std::uint64_t required_scans = required_successful_scan_count();
        if (!common_identity_accepted_() || required_scans == 0u ||
            geometry_retry_count > (std::numeric_limits<std::uint64_t>::max)() - required_scans ||
            producer_scan_count != geometry_retry_count + required_scans) {
            return false;
        }
        return selected_route_accepted_(requires_backend_work_());
    }

    /// Check a direct fragment build: one source scan with no geometry retry or
    /// publication binding Accept the  registered algorithm identity or the
    /// unbound compatibility path with k=15 and w=10.
    [[nodiscard]] bool direct_build_accepted() const noexcept {
        const bool legacy_unbound_build =
            kmer_size == 15u && window_size == 10u &&
            !route_snapshot_present_ &&
            algorithm_identity == minimizer_generation_algorithm_identity{};
        const bool registered_identity_build =
            route_snapshot_present_ && route_snapshot_matches_() &&
            algorithm_identity.runtime_kmer_size == kmer_size &&
            algorithm_identity.runtime_window_size == window_size &&
            is_valid_minimizer_generation_algorithm_identity(
                algorithm_identity,
                minimizer_producer_backend_identity_for_backend(selected_backend));
        return common_identity_accepted_() && producer_scan_count == 1u &&
               geometry_retry_count == 0u &&
               generation_layout == minimizer_generation_layout::invalid &&
               (legacy_unbound_build || registered_identity_build) &&
               !publication_binding_present_ &&
               selected_route_accepted_(requires_backend_work_());
    }

    /// Check that unchanged execution stats matcd this encoded generation's publication binding.
    [[nodiscard]] bool
    publication_accepted(const minimizer_generation_publication_binding& expected) const noexcept {
        return is_accepted() && route_snapshot_matches_() && publication_binding_present_ &&
               generation_layout != minimizer_generation_layout::invalid &&
               generation_layout == expected.layout && expected.valid() &&
               publication_binding_ == expected;
    }

  private:
    /// Check backend, scheme, target length, and minimizer parameters.
    [[nodiscard]] bool common_identity_accepted_() const noexcept {
        const minimizer_producer_identity producer =
            minimizer_producer_backend_identity_for_backend(selected_backend);
        const bool algorithm_identity_accepted =
            generation_layout == minimizer_generation_layout::invalid ||
            (is_valid_minimizer_generation_algorithm_identity(algorithm_identity, producer) &&
             algorithm_identity.runtime_kmer_size == kmer_size &&
             algorithm_identity.runtime_window_size == window_size);
        return backend_bound && !saturated && target_length != 0u && window_size != 0u &&
               kmer_size != 0u && algorithm_identity_accepted &&
               minimizer_scheme == minimizer_scheme_identity_for_backend(selected_backend);
    }

    /// Test whether the target is long enough to require a minimizer backend invocation.
    [[nodiscard]] bool requires_backend_work_() const noexcept {
        const std::uint64_t minimum_work_bases =
            static_cast<std::uint64_t>(window_size) + kmer_size - 1u;
        return target_length >= minimum_work_bases;
    }

    /// Check that only the selected backend recorded work and its counters agree.
    /// Short targets may complete without a kernel call.
    [[nodiscard]] bool selected_route_accepted_(bool require_backend_work) const noexcept {
        const bool no_exact = exact_simd_core_runs == 0 && !exact_simd_stats_present &&
                              !reference_simd_stats_present;
        const bool no_bounded = bounded_nthash_core_runs == 0 && !packed_bounded_stats_present;
        const bool no_vulkan = vulkan_core_runs == 0 && vulkan_stats.completed_roundtrips == 0 &&
                               vulkan_stats.downloaded_minimizers == 0 &&
                               vulkan_stats.gpu_window_splits == 0;

        switch (selected_backend) {
        case minimizer_sketch_backend::scalar:
            return exact_simd_core_runs == 0 && bounded_nthash_core_runs == 0 &&
                   vulkan_core_runs == 0 && no_exact && no_bounded && no_vulkan &&
                   (!require_backend_work || scalar_core_runs != 0);
        case minimizer_sketch_backend::exact_simd:
            if (scalar_core_runs != 0 || bounded_nthash_core_runs != 0 || vulkan_core_runs != 0 ||
                !no_bounded || !no_vulkan) {
                return false;
            }
            if (exact_simd_core_runs == 0) {
                return !require_backend_work && !exact_simd_stats_present &&
                       !reference_simd_stats_present;
            }
            if (algorithm_identity.effective_algorithm ==
                bioseq::simd::minimizer_algorithm::simd_minimap2_reference) {
                return !exact_simd_stats_present && reference_simd_stats_present &&
                       reference_simd_stats.is_internally_consistent(
                           require_backend_work, algorithm_identity.packed_storage_bits / 8u);
            }
            return (generation_layout == minimizer_generation_layout::invalid ||
                    algorithm_identity.effective_algorithm ==
                        bioseq::simd::minimizer_algorithm::two_stack_nthash) &&
                   exact_simd_stats_present && !reference_simd_stats_present &&
                   exact_simd_stats.is_internally_consistent(require_backend_work);
        case minimizer_sketch_backend::bounded_nthash_simd:
            return scalar_core_runs == 0 && exact_simd_core_runs == 0 && vulkan_core_runs == 0 &&
                   no_exact && no_vulkan &&
                   (bounded_nthash_core_runs == 0
                        ? (!require_backend_work && !packed_bounded_stats_present)
                        : (packed_bounded_stats_present &&
                           packed_bounded_status == packed_bounded_nthash_index_status::ok &&
                           packed_bounded_stats.is_internally_consistent(require_backend_work)));
        case minimizer_sketch_backend::vulkan_segment:
            return scalar_core_runs == 0 && exact_simd_core_runs == 0 &&
                   bounded_nthash_core_runs == 0 && no_exact && no_bounded &&
                   (vulkan_core_runs == 0
                        ? (!require_backend_work && vulkan_stats.is_internally_consistent(false))
                        : vulkan_stats.is_internally_consistent(require_backend_work));
        }
        return false;
    }
    template <class p_storage> friend struct minimizers_per_fragment_index;
    template <sequence_alphabet p_alphabet>
    friend class minimizer_fragment_generation_replay_producer;

    /// Private copy of execution fields used to detect later receipt changes.
    struct route_snapshot {
        minimizer_sketch_backend selected_backend = minimizer_sketch_backend::scalar;
        minimizer_scheme_identity minimizer_scheme{};
        minimizer_generation_algorithm_identity algorithm_identity{};
        std::uint64_t target_length = 0;
        std::uint16_t window_size = 0;
        std::uint16_t kmer_size = 0;
        std::uint64_t producer_scan_count = 0;
        std::uint64_t geometry_retry_count = 0;
        minimizer_generation_layout generation_layout = minimizer_generation_layout::invalid;
        std::uint64_t short_core_runs = 0;
        std::uint64_t scalar_core_runs = 0;
        std::uint64_t exact_simd_core_runs = 0;
        std::uint64_t bounded_nthash_core_runs = 0;
        std::uint64_t vulkan_core_runs = 0;
        exact_simd_index_execution_stats exact_simd_stats{};
        bool exact_simd_stats_present = false;
        reference_simd_index_execution_stats reference_simd_stats{};
        bool reference_simd_stats_present = false;
        packed_bounded_nthash_index_status packed_bounded_status =
            packed_bounded_nthash_index_status::invalid_state;
        packed_bounded_nthash_index_execution_stats packed_bounded_stats{};
        bool packed_bounded_stats_present = false;
        bioseq::gpu::vulkan::vulkan_segment_pair_execution_stats vulkan_stats{};
        bool backend_bound = false;
        bool saturated = false;
    } route_snapshot_{};
    bool route_snapshot_present_ = false;

    minimizer_generation_publication_binding publication_binding_{};
    bool publication_binding_present_ = false;

    /// Compare the Vulkan completion, output, split, and  overflow counters.
    [[nodiscard]] static bool same_vulkan_stats_(
        const bioseq::gpu::vulkan::vulkan_segment_pair_execution_stats& left,
        const bioseq::gpu::vulkan::vulkan_segment_pair_execution_stats& right) noexcept {
        return left.completed_roundtrips == right.completed_roundtrips &&
               left.downloaded_minimizers == right.downloaded_minimizers &&
               left.gpu_window_splits == right.gpu_window_splits &&
               left.saturated == right.saturated;
    }

    /// Save, execution fields before attaching generation metadata.
    void capture_route_snapshot_() noexcept {
        route_snapshot_ = route_snapshot{
            .selected_backend = selected_backend,
            .minimizer_scheme = minimizer_scheme,
            .algorithm_identity = algorithm_identity,
            .target_length = target_length,
            .window_size = window_size,
            .kmer_size = kmer_size,
            .producer_scan_count = producer_scan_count,
            .geometry_retry_count = geometry_retry_count,
            .generation_layout = generation_layout,
            .short_core_runs = short_core_runs,
            .scalar_core_runs = scalar_core_runs,
            .exact_simd_core_runs = exact_simd_core_runs,
            .bounded_nthash_core_runs = bounded_nthash_core_runs,
            .vulkan_core_runs = vulkan_core_runs,
            .exact_simd_stats = exact_simd_stats,
            .exact_simd_stats_present = exact_simd_stats_present,
            .reference_simd_stats = reference_simd_stats,
            .reference_simd_stats_present = reference_simd_stats_present,
            .packed_bounded_status = packed_bounded_status,
            .packed_bounded_stats = packed_bounded_stats,
            .packed_bounded_stats_present = packed_bounded_stats_present,
            .vulkan_stats = vulkan_stats,
            .backend_bound = backend_bound,
            .saturated = saturated,
        };
        route_snapshot_present_ = true;
    }

    /// Detect receipt mutation after the route snapshot was captured.
    [[nodiscard]] bool route_snapshot_matches_() const noexcept {
        const route_snapshot& value = route_snapshot_;
        return route_snapshot_present_ && selected_backend == value.selected_backend &&
               minimizer_scheme == value.minimizer_scheme &&
               algorithm_identity == value.algorithm_identity &&
               target_length == value.target_length &&
               window_size == value.window_size && kmer_size == value.kmer_size &&
               producer_scan_count == value.producer_scan_count &&
               geometry_retry_count == value.geometry_retry_count &&
               generation_layout == value.generation_layout &&
               short_core_runs == value.short_core_runs &&
               scalar_core_runs == value.scalar_core_runs &&
               exact_simd_core_runs == value.exact_simd_core_runs &&
               bounded_nthash_core_runs == value.bounded_nthash_core_runs &&
               vulkan_core_runs == value.vulkan_core_runs &&
               exact_simd_stats == value.exact_simd_stats &&
               exact_simd_stats_present == value.exact_simd_stats_present &&
               reference_simd_stats == value.reference_simd_stats &&
               reference_simd_stats_present == value.reference_simd_stats_present &&
               packed_bounded_status == value.packed_bounded_status &&
               packed_bounded_stats == value.packed_bounded_stats &&
               packed_bounded_stats_present == value.packed_bounded_stats_present &&
               same_vulkan_stats_(vulkan_stats, value.vulkan_stats) &&
               backend_bound == value.backend_bound && saturated == value.saturated;
    }

    /// Mark the generation binding valid only when it matches the saved execution fields.
    void bind_publication_(const encoded_grouped_soa_generation& generation) noexcept {
        const minimizer_producer_identity backend =
            minimizer_producer_backend_identity_for_backend(selected_backend);
        publication_binding_ = make_minimizer_generation_publication_binding(generation);
        publication_binding_present_ =
            publication_binding_.valid() && route_snapshot_matches_() &&
            generation_layout == generation.manifest.layout &&
            target_length == generation.manifest.target_length &&
            window_size == generation.manifest.window_size &&
            kmer_size == generation.manifest.kmer_size &&
            minimizer_scheme.id == generation.manifest.minimizer_scheme_id &&
            minimizer_scheme.revision == generation.manifest.minimizer_scheme_revision &&
            backend.id == generation.manifest.producer_backend_id &&
            backend.revision == generation.manifest.producer_identity_revision &&
            algorithm_identity == generation.manifest.algorithm_identity;
    }
};

/// Rescan a borrowed sequence and emit minimizers sorted within each persisted segment.
/// Read w+k-2 context bases around each core, then keep only occurrences
/// whose positions belong to that core. Sort and deduplicate each run. If a
/// segment crosses physical fragment boundaries, merge its runs before emission.
/// Vulkan can batch adjacent segments while  keeping their output separate.
///
/// @tparam p_alphabet Alphabet of the borrowed sequence.
template <sequence_alphabet p_alphabet> class minimizer_fragment_generation_replay_producer {
  private:
    using sequence_type = bioseq::storage::lazy_sequence_view<p_alphabet>;
    using traits = typename sequence_type::traits;

    /// Run and event indices for the next item in the merge heap.
    struct cursor {
        std::size_t run = 0;
        std::size_t index = 0;
    };

    sequence_type sequence_;
    minimizer_sketch_backend backend_ = minimizer_sketch_backend::scalar;
    const bioseq::minimizers::minimizer_kernels* kernels_ = nullptr;
    minimizer_generation_algorithm_identity algorithm_identity_{};
    std::uint8_t alphabet_code_ = bioseq::minimizers::k_unbound_packed_alphabet_code;
    std::uint16_t window_size_ = 0;
    std::uint16_t kmer_size_ = 0;
    std::size_t gpu_run_buffer_bytes_ = 0;
    cooperative_cancellation_checkpoint cancellation_checkpoint_{};
    std::uint64_t scan_count_ = 0;
    std::uint64_t geometry_retry_count_ = 0;
    minimizer_generation_layout generation_layout_ = minimizer_generation_layout::invalid;
    std::uint64_t last_scan_span_ = 0;
    bool has_last_scan_span_ = false;
    std::uint64_t peak_buffered_events_ = 0;
    std::uint64_t peak_buffered_core_bp_ = 0;
    std::uint64_t peak_active_fragment_runs_ = 0;
    std::uint64_t peak_bounded_materialized_window_bp_ = 0;
    exact_simd_index_execution_stats exact_simd_stats_{};
    bool exact_simd_stats_present_ = false;
    reference_simd_index_execution_stats reference_simd_stats_{};
    bool reference_simd_stats_present_ = false;
    packed_bounded_nthash_index_status packed_bounded_status_ =
        packed_bounded_nthash_index_status::invalid_state;
    packed_bounded_nthash_index_execution_stats packed_bounded_stats_{};
    bool packed_bounded_stats_present_ = false;
    std::uint64_t short_core_runs_ = 0;
    std::uint64_t scalar_core_runs_ = 0;
    std::uint64_t exact_simd_core_runs_ = 0;
    std::uint64_t bounded_nthash_core_runs_ = 0;
    std::uint64_t vulkan_core_runs_ = 0;
    bioseq::gpu::vulkan::vulkan_segment_pair_execution_stats vulkan_stats_{};
    bool route_stats_saturated_ = false;

    /// Check for cancellation on the thread coordinating this synchronous producer.
    void cancellation_checkpoint() const {
        cancellation_checkpoint_();
    }

    /// Increment a route counter and flag overflow instead of wlapping
    void increment_route_counter_(std::uint64_t& counter) noexcept {
        if (counter == (std::numeric_limits<std::uint64_t>::max)()) {
            route_stats_saturated_ = true;
            return;
        }
        ++counter;
    }

    /// Add bounded ntHash counters and remember the latest status.
    void
    record_packed_bounded_result(const packed_bounded_nthash_index_bridge_result& result) noexcept {
        packed_bounded_status_ = result.status;
        packed_bounded_stats_.merge(result.stats);
        packed_bounded_stats_present_ = true;
    }

    /// Sort one core's events by hash and position/strand, then remove duplicates.
    static void canonicalize_core_run_(
        std::vector<canonical_minimizer_event>& run) {
        std::ranges::sort(run, {}, [](const canonical_minimizer_event& event) {
            return std::pair{event.hash, event.encoded_position};
        });
        // Adjacent tied windows can report the same minimizer. Store  one
        // event for each hash, position, and strand. Disjoint core ranges
        // give each occurrence a single owner, so duplicates can be removed
        // within each run before the encoder checks the final stream.
        run.erase(std::ranges::unique(run).begin(), run.end());
    }

    /// Input context range and output core bounds for one Vulkan upload.
    struct vulkan_core_geometry {
        std::uint64_t collect_lo = 0u;
        std::uint64_t collect_hi = 0u;
        std::uint64_t emit_base = 0u;
        std::uint64_t core_lo = 0u;
        std::uint64_t core_hi = 0u;
        std::uint64_t output_segment_lo = 0u;
        std::uint64_t next_core_lo = 0u;
    };

    /// A nonempty core's minimizers and the persisted segment receiving them.
    struct vulkan_segment_core_run {
        std::uint64_t output_segment_lo = 0u;
        std::vector<canonical_minimizer_event> events;
    };

    /// Merge a segment's runs by hash and position/strand and send events to the receiver.
    /// Callers omit empty runs. A receiver error stops emission.
    [[nodiscard]] static std::expected<void, minimizer_generation_encoding_error>
    emit_sorted_core_runs_(
        const std::vector<std::vector<canonical_minimizer_event>>& runs,
        canonical_minimizer_event_receiver& receiver) {
        if (runs.empty()) {
            return {};
        }
        if (runs.size() == 1u) {
            for (const canonical_minimizer_event& event : runs.front()) {
                if (auto accepted = receiver(event); !accepted) {
                    return accepted;
                }
            }
            return {};
        }

        // Allocate all heap cursors up front. This avoids the Clang 22
        // libc++ append miscompilation seen  with this module's cursor type.
        // Heap operations use only the active prefix, ordered by hash and
        // encoded position.
        const auto after = [&runs](const cursor& lhs, const cursor& rhs) {
            const auto& left = runs[lhs.run][lhs.index];
            const auto& right = runs[rhs.run][rhs.index];
            return std::pair{left.hash, left.encoded_position} >
                   std::pair{right.hash, right.encoded_position};
        };
        std::vector<cursor> heap(runs.size());
        std::size_t active_cursor_count = 0u;
        for (std::size_t run = 0; run < runs.size(); ++run) {
            heap[active_cursor_count] = cursor{.run = run, .index = 0};
            ++active_cursor_count;
            std::push_heap(heap.begin(), heap.begin() + active_cursor_count, after);
        }
        while (active_cursor_count != 0u) {
            std::pop_heap(heap.begin(), heap.begin() + active_cursor_count, after);
            cursor& current_cursor = heap[active_cursor_count - 1u];
            if (auto accepted = receiver(runs[current_cursor.run][current_cursor.index]);
                !accepted) {
                return accepted;
            }
            ++current_cursor.index;
            if (current_cursor.index < runs[current_cursor.run].size()) {
                std::push_heap(heap.begin(), heap.begin() + active_cursor_count, after);
            } else {
                --active_cursor_count;
            }
        }
        return {};
    }

    /**
     * Collect Vulkan minimizers for cores in [segment_lo, segment_hi).
     *
     * Split cores at both physical fragment and persisted segment boundaries.
     * Each upload includes its own context, whose storage stays alive until
     * the, batch completes. Check returned slices, keep positions belonging to
     * each core, and sort and deduplicate the results.
     *
     * @tparam p_minimizer_type Record type selected for the largest undividual
     *     upload window before allocation or dispatch.
     * @param segment_lo Inclusive range start.
     * @param segment_hi Exclusive range end
     * @param fragment_bp Physical  fragment width in base pairs.
     * @param output_segment_span Persisted segment width in base pairs.
     * @return Nonempty core runs tagged with their persisted segment start.
     */
    template <class p_minimizer_type>
    [[nodiscard]] std::expected<
        std::vector<vulkan_segment_core_run>,
        minimizer_generation_encoding_error>
    collect_vulkan_segment_runs_(
        std::uint64_t segment_lo,
        std::uint64_t segment_hi,
        std::uint64_t fragment_bp,
        std::uint64_t output_segment_span) {
        using minimizer_type = p_minimizer_type;
        using canonical_window =
            bioseq::gpu::vulkan::admitted_sequence_detail::canonical_u32_window<
                p_alphabet>;
        using canonical_window_batch = bioseq::gpu::vulkan::
            admitted_sequence_detail::canonical_u32_window_batch<p_alphabet>;
        using upload_instance =
            bioseq::gpu::vulkan::admitted_sequence_detail::borrowed_u32_instance<
                p_alphabet>;
        using upload_range = compact_sequence_range<upload_instance>;

        const std::uint64_t target_length = sequence_.bp_size();
        const std::uint64_t halo =
            static_cast<std::uint64_t>(window_size_) + kmer_size_ - 2u;
        const std::uint64_t minimum_bases =
            static_cast<std::uint64_t>(window_size_) + kmer_size_ - 1u;
        const bioseq::gpu::vulkan::vulkan_rank_lut rank_lut =
            bioseq::gpu::vulkan::make_vulkan_rank_lut<upload_range>();
        if (fragment_bp == 0u || output_segment_span == 0u
            || segment_lo > segment_hi || segment_hi > target_length) {
            return std::unexpected(
                minimizer_generation_encoding_error::arithmetic_overflow);
        }
        std::vector<vulkan_segment_core_run> completed_runs;
        std::optional<canonical_window> pending_owner;
        std::optional<vulkan_core_geometry> pending_geometry;
        std::uint64_t core_cursor = segment_lo;

        const auto geometry_for_core = [=](std::uint64_t core_lo)
            -> std::expected<vulkan_core_geometry,
                             minimizer_generation_encoding_error> {
            if (core_lo >= segment_hi || core_lo >= target_length) {
                return std::unexpected(
                    minimizer_generation_encoding_error::arithmetic_overflow);
            }
            const std::uint64_t fragment_lo =
                (core_lo / fragment_bp) * fragment_bp;
            const std::uint64_t fragment_hi = std::min(
                target_length,
                bioseq::common::saturating_add(fragment_lo, fragment_bp));
            const std::uint64_t output_segment_lo =
                (core_lo / output_segment_span) * output_segment_span;
            const std::uint64_t output_segment_hi = std::min(
                target_length,
                bioseq::common::saturating_add(output_segment_lo,
                                                output_segment_span));
            const std::uint64_t core_hi =
                std::min({segment_hi, fragment_hi, output_segment_hi});
            if (core_hi <= core_lo) {
                return std::unexpected(
                    minimizer_generation_encoding_error::arithmetic_overflow);
            }
            const std::uint64_t collect_lo =
                core_lo >= halo ? core_lo - halo : 0u;
            const std::uint64_t collect_hi =
                core_hi > target_length - std::min(target_length, halo)
                    ? target_length
                    : core_hi + halo;
            return vulkan_core_geometry{
                .collect_lo = collect_lo,
                .collect_hi = collect_hi,
                .emit_base = collect_lo,
                .core_lo = core_lo,
                .core_hi = core_hi,
                .output_segment_lo = output_segment_lo,
                .next_core_lo = core_hi,
            };
        };

        while (core_cursor < segment_hi) {
            auto plan = bioseq::gpu::vulkan::
                make_minimap_sketch_data_vulkan_upload_batch_admission_plan<
                    minimizer_type>(
                        rank_lut,
                        window_size_,
                        kmer_size_,
                        bioseq::gpu::vulkan::vulkan_sketch_host_materialization::
                            host_and_device);
            if (!plan) {
                pending_owner.reset();
                pending_geometry.reset();
                auto geometry = geometry_for_core(core_cursor);
                if (!geometry) {
                    return std::unexpected(geometry.error());
                }
                auto run = collect_core_run(geometry->core_lo, geometry->core_hi);
                if (!run) {
                    return std::unexpected(run.error());
                }
                if (!run->empty()) {
                    completed_runs.push_back(vulkan_segment_core_run{
                        .output_segment_lo = geometry->output_segment_lo,
                        .events = std::move(*run),
                    });
                }
                core_cursor = geometry->next_core_lo;
                continue;
            }

            canonical_window_batch source_owners;
            bioseq::gpu::vulkan::sequence_upload_batch upload_batch;
            std::vector<vulkan_core_geometry> geometries;
            std::uint64_t candidate_core_lo = core_cursor;

            while (candidate_core_lo < segment_hi) {
                vulkan_core_geometry geometry{};
                std::optional<canonical_window> candidate_owner;
                if (pending_owner) {
                    if (!pending_geometry) {
                        throw bioseq::gpu::vulkan::vulkan_runtime_error{
                            "Vulkan fragment batch retained an owner without geometry"};
                    }
                    geometry = *pending_geometry;
                    if (geometry.core_lo != candidate_core_lo) {
                        throw bioseq::gpu::vulkan::vulkan_runtime_error{
                            "Vulkan fragment batch retained non-contiguous geometry"};
                    }
                    candidate_owner.emplace(std::move(*pending_owner));
                    pending_owner.reset();
                    pending_geometry.reset();
                } else {
                    auto next_geometry = geometry_for_core(candidate_core_lo);
                    if (!next_geometry) {
                        return std::unexpected(next_geometry.error());
                    }
                    geometry = *next_geometry;
                    if (geometry.collect_hi <= geometry.collect_lo
                        || geometry.collect_hi - geometry.collect_lo
                            < minimum_bases) {
                        break;
                    }
                    bioseq::gpu::vulkan::vulkan_sketch_decline decline{
                        .bp_size = geometry.collect_hi - geometry.collect_lo,
                        .window_size = window_size_,
                        .kmer_size = kmer_size_,
                    };
                    auto window = bioseq::gpu::vulkan::admitted_sequence_detail::
                        prepare_canonical_u32_window(
                            sequence_,
                            static_cast<sequence_size_t>(geometry.collect_lo),
                            static_cast<sequence_size_t>(geometry.collect_hi),
                            &decline);
                    if (!window) {
                        break;
                    }
                    candidate_owner.emplace(std::move(*window));
                }

                auto upload = bioseq::gpu::vulkan::admitted_sequence_detail::
                    with_canonical_u32_upload_source<p_alphabet>(
                        *candidate_owner,
                        []<class p_range>(p_range& resident) {
                            return bioseq::gpu::vulkan::make_sequence_upload_view(
                                resident);
                        });
                if (!bioseq::gpu::vulkan::
                        try_append_minimap_sketch_data_vulkan_upload(
                            *plan, upload)) {
                    pending_owner.emplace(std::move(*candidate_owner));
                    pending_geometry = geometry;
                    break;
                }
                source_owners.push_back(std::move(*candidate_owner));
                upload_batch.push_back(std::move(upload));
                geometries.push_back(geometry);
                if (geometry.next_core_lo <= candidate_core_lo) {
                    return std::unexpected(
                        minimizer_generation_encoding_error::arithmetic_overflow);
                }
                candidate_core_lo = geometry.next_core_lo;
            }

            if (geometries.empty()) {
                pending_owner.reset();
                pending_geometry.reset();
                auto geometry = geometry_for_core(core_cursor);
                if (!geometry) {
                    return std::unexpected(geometry.error());
                }
                auto run = collect_core_run(geometry->core_lo, geometry->core_hi);
                if (!run) {
                    return std::unexpected(run.error());
                }
                if (!run->empty()) {
                    completed_runs.push_back(vulkan_segment_core_run{
                        .output_segment_lo = geometry->output_segment_lo,
                        .events = std::move(*run),
                    });
                }
                core_cursor = geometry->next_core_lo;
                continue;
            }

            const std::size_t accepted_count = plan->accepted_upload_count();
            if (accepted_count != geometries.size()
                || !plan->bind_accepted_uploads(upload_batch.view())) {
                throw bioseq::gpu::vulkan::vulkan_runtime_error{
                    "Vulkan fragment batch admission did not bind its exact prefix"};
            }
            auto batch_result = bioseq::gpu::vulkan::
                execute_admitted_minimap_sketch_data_vulkan_upload_batch<
                    minimizer_type>(std::move(*plan));
            if (!batch_result
                || batch_result->materialization
                    != bioseq::gpu::vulkan::vulkan_sketch_host_materialization::
                        host_and_device
                || !batch_result->dispatch_stats.is_internally_consistent()
                || batch_result->dispatch_stats.completed_roundtrips() != 1u
                || batch_result->query_records.size() != accepted_count
                || batch_result->device_minimizer_count
                    != batch_result->host_minimizers.size()) {
                throw bioseq::gpu::vulkan::vulkan_runtime_error{
                    "Vulkan fragment batch returned an invalid completed result"};
            }
            // GPU execution has finished and its result shape is valid.
            // Cancellation can now release the result and source storage safely.
            cancellation_checkpoint();

            std::vector<std::vector<canonical_minimizer_event>> pending_runs(
                accepted_count);
            std::uint64_t record_cursor = 0u;
            for (std::size_t core_index = 0u;
                 core_index < accepted_count;
                 ++core_index) {
                const auto& record = batch_result->query_records[core_index];
                const vulkan_core_geometry& geometry = geometries[core_index];
                const std::uint64_t expected_query_length =
                    geometry.collect_hi - geometry.collect_lo;
                const std::uint64_t record_end = bioseq::common::saturating_add(
                    static_cast<std::uint64_t>(record.minimizer_offset),
                    static_cast<std::uint64_t>(record.minimizer_count));
                if (record.flags != 0u
                    || record.minimizer_offset != record_cursor
                    || record.query_len != expected_query_length
                    || record_end > batch_result->host_minimizers.size()) {
                    throw bioseq::gpu::vulkan::vulkan_runtime_error{
                        "Vulkan fragment batch query partition is invalid"};
                }
                auto& run = pending_runs[core_index];
                run.reserve(record.minimizer_count);
                for (std::uint64_t minimizer_index = record.minimizer_offset;
                     minimizer_index < record_end;
                     ++minimizer_index) {
                    const minimizer_type& minimizer =
                        batch_result->host_minimizers[
                            static_cast<std::size_t>(minimizer_index)];
                    if (minimizer.last_pos() >= record.query_len
                        || minimizer.last_pos()
                            > (std::numeric_limits<std::uint64_t>::max)()
                                - geometry.emit_base) {
                        throw bioseq::gpu::vulkan::vulkan_runtime_error{
                            "Vulkan fragment batch minimizer position is invalid"};
                    }
                    const std::uint64_t position =
                        geometry.emit_base
                        + static_cast<std::uint64_t>(minimizer.last_pos());
                    if (position < geometry.core_lo || position >= geometry.core_hi) {
                        continue;
                    }
                    run.push_back(canonical_minimizer_event{
                        .hash = minimizer.kmer(),
                        .encoded_position =
                            (position << 1u)
                            | static_cast<std::uint64_t>(minimizer.strand()),
                    });
                }
                canonicalize_core_run_(run);
                record_cursor = record_end;
            }
            if (record_cursor != batch_result->host_minimizers.size()) {
                throw bioseq::gpu::vulkan::vulkan_runtime_error{
                    "Vulkan fragment batch partitions do not cover host output"};
            }

            bioseq::gpu::vulkan::vulkan_segment_pair_execution_stats batch_stats{};
            batch_stats.record_completed_roundtrip(
                batch_result->host_minimizers.size());
            vulkan_stats_.merge(batch_stats);
            route_stats_saturated_ =
                route_stats_saturated_ || vulkan_stats_.saturated;
            for (std::size_t core_index = 0u;
                 core_index < pending_runs.size();
                 ++core_index) {
                auto& run = pending_runs[core_index];
                increment_route_counter_(vulkan_core_runs_);
                if (!run.empty()) {
                    completed_runs.push_back(vulkan_segment_core_run{
                        .output_segment_lo =
                            geometries[core_index].output_segment_lo,
                        .events = std::move(run),
                    });
                }
            }
            core_cursor = geometries.back().next_core_lo;
            // Check cancellation after validating and storing the completed batch.
            cancellation_checkpoint();
        }
        return completed_runs;
    }

    /// Collect [core_lo, core_hi) with w+k-2 context bases on each side, clipped
    /// to the sequence bounds. Keep core positions, then sort and deduplicate.
    /// Backend mismatches and exact SIMD failures return storage_contract_mismatch.
    /// Bounded ntHash, Vulkan, and  cancellation failures can throw.
    [[nodiscard]] std::expected<std::vector<canonical_minimizer_event>,
                                minimizer_generation_encoding_error>
    collect_core_run(std::uint64_t core_lo, std::uint64_t core_hi) {
        const std::uint64_t target_length = sequence_.bp_size();
        const std::uint64_t halo = static_cast<std::uint64_t>(window_size_) + kmer_size_ - 2u;
        const std::uint64_t collect_lo = core_lo >= halo ? core_lo - halo : 0u;
        const std::uint64_t collect_hi = core_hi > target_length - std::min(target_length, halo)
                                             ? target_length
                                             : core_hi + halo;
        const std::uint64_t emit_base = collect_lo;
        const std::uint64_t minimum_bases =
            static_cast<std::uint64_t>(window_size_) + kmer_size_ - 1u;
        std::vector<canonical_minimizer_event> run;
        if (collect_hi <= emit_base || collect_hi - emit_base < minimum_bases) {
            increment_route_counter_(short_core_runs_);
            cancellation_checkpoint();
            return run;
        }

        if (backend_ == minimizer_sketch_backend::vulkan_segment) {
            increment_route_counter_(vulkan_core_runs_);
            bioseq::gpu::vulkan::vulkan_segment_pair_execution_stats core_stats{};
            auto pairs =
                bioseq::gpu::vulkan::collect_sorted_minimizer_pairs_vulkan_segment_or_throw(
                    sequence_,
                    static_cast<sequence_size_t>(collect_lo),
                    static_cast<sequence_size_t>(collect_hi),
                    static_cast<sequence_size_t>(emit_base),
                    static_cast<sequence_size_t>(core_lo),
                    static_cast<sequence_size_t>(core_hi),
                    window_size_,
                    kmer_size_,
                    &core_stats);
            vulkan_stats_.merge(core_stats);
            route_stats_saturated_ = route_stats_saturated_ || vulkan_stats_.saturated;
            run.reserve(pairs.pairs.size());
            for (const auto& pair : pairs.pairs) {
                run.push_back(canonical_minimizer_event{
                    .hash = pair.first,
                    .encoded_position = pair.second,
                });
            }
        } else if (backend_ == minimizer_sketch_backend::scalar) {
            increment_route_counter_(scalar_core_runs_);
            const auto identity_layout =
                select_minimizer_generation_physical_layout<p_alphabet>(
                    algorithm_identity_.requested_algorithm,
                    k_minimizer_fragment_algorithm_position_extent_bp,
                    algorithm_identity_.packed_storage_bits,
                    kmer_size_,
                    window_size_);
            if (!identity_layout) {
                return std::unexpected(
                    minimizer_generation_encoding_error::storage_contract_mismatch);
            }
            const auto selected_identity =
                make_minimizer_generation_algorithm_identity(*identity_layout);
            if (!selected_identity || *selected_identity != algorithm_identity_) {
                return std::unexpected(
                    minimizer_generation_encoding_error::storage_contract_mismatch);
            }
            // The persisted identity covers a 32,768-base fragment. Collection
            // also includes context, so the scalar run may need a record
            // with a wider position field.
            const auto transient_layout =
                select_minimizer_generation_physical_layout<p_alphabet>(
                    algorithm_identity_.requested_algorithm,
                    collect_hi - emit_base,
                    algorithm_identity_.packed_storage_bits,
                    kmer_size_,
                    window_size_);
            if (!transient_layout) {
                return std::unexpected(
                    minimizer_generation_encoding_error::storage_contract_mismatch);
            }
            try {
                (void)bioseq::minimizers::visit_minimizer_physical_layout(
                    *transient_layout,
                    [this,
                     collect_lo,
                     collect_hi,
                     emit_base,
                     core_lo,
                     core_hi,
                     &run]<class p_minimizer>() {
                        struct canonical_event_collector {
                            std::vector<canonical_minimizer_event>* output = nullptr;
                            std::uint64_t emit_base = 0u;
                            std::uint64_t core_lo = 0u;
                            std::uint64_t core_hi = 0u;

                            /** Keep scalar minimizers within this core and convert their positions to global coordinates. */
                            void collect(const p_minimizer& minimizer) {
                                const std::uint64_t position =
                                    emit_base +
                                    static_cast<std::uint64_t>(minimizer.last_pos());
                                if (position < core_lo || position >= core_hi) {
                                    return;
                                }
                                output->push_back(canonical_minimizer_event{
                                    .hash = minimizer.kmer(),
                                    .encoded_position =
                                        (position << 1u) |
                                        static_cast<std::uint64_t>(minimizer.strand()),
                                });
                            }
                        } collector{
                            .output = &run,
                            .emit_base = emit_base,
                            .core_lo = core_lo,
                            .core_hi = core_hi,
                        };
                        bioseq::simd::collect_minimizers_segment_scalar<p_minimizer>(
                            collector,
                            sequence_,
                            static_cast<sequence_size_t>(collect_lo),
                            static_cast<sequence_size_t>(collect_hi),
                            window_size_,
                            kmer_size_);
                    });
            } catch (const std::invalid_argument&) {
                return std::unexpected(
                    minimizer_generation_encoding_error::storage_contract_mismatch);
            }
        } else {
            sketch_collector collector;
            if (backend_ == minimizer_sketch_backend::bounded_nthash_simd) {
                increment_route_counter_(bounded_nthash_core_runs_);
                std::uint64_t materialized_window_bp = 0u;
                const auto bounded_result = collect_packed_bounded_nthash_index_lazy(
                    sequence_,
                    alphabet_code_,
                    static_cast<sequence_size_t>(collect_lo),
                    static_cast<sequence_size_t>(collect_hi),
                    collect_hi - emit_base,
                    window_size_,
                    kmer_size_,
                    kernels_,
                    collector,
                    &materialized_window_bp,
                    algorithm_identity_.packed_storage_bits);
                peak_bounded_materialized_window_bp_ =
                    std::max(peak_bounded_materialized_window_bp_, materialized_window_bp);
                record_packed_bounded_result(bounded_result);
                if (bounded_result.status != packed_bounded_nthash_index_status::ok) {
                    throw packed_bounded_nthash_index_runtime_error{
                        bounded_result.status, "minimizer fragment generation replay"};
                }
            } else if (backend_ == minimizer_sketch_backend::exact_simd) {
                increment_route_counter_(exact_simd_core_runs_);
                try {
                    if (algorithm_identity_.effective_algorithm ==
                        bioseq::simd::minimizer_algorithm::simd_minimap2_reference) {
                        const auto selected_layout =
                            select_minimizer_generation_physical_layout<p_alphabet>(
                                algorithm_identity_.requested_algorithm,
                                k_minimizer_fragment_algorithm_position_extent_bp,
                                algorithm_identity_.packed_storage_bits,
                                kmer_size_,
                                window_size_);
                        if (!selected_layout) {
                            return std::unexpected(
                                minimizer_generation_encoding_error::storage_contract_mismatch);
                        }
                        const auto selected_identity =
                            make_minimizer_generation_algorithm_identity(*selected_layout);
                        if (!selected_identity || *selected_identity != algorithm_identity_) {
                            return std::unexpected(
                                minimizer_generation_encoding_error::storage_contract_mismatch);
                        }
                        const auto stats = bioseq::minimizers::visit_minimizer_physical_layout(
                            *selected_layout,
                            [this,
                             core_lo,
                             core_hi,
                             target_length,
                             halo,
                             &run]<class p_minimizer>() {
                                const std::uint64_t local_position_capacity =
                                    static_cast<std::uint64_t>(
                                        bioseq::minimizers::
                                            k_max_minimizer_sequence_length<p_minimizer>);
                                if (halo > (std::numeric_limits<std::uint64_t>::max)() / 2u ||
                                    local_position_capacity <= 2u * halo) {
                                    throw std::invalid_argument(
                                        "reference fragment owner cannot represent its halo");
                                }
                                const std::uint64_t maximum_core_bp =
                                    local_position_capacity - 2u * halo;
                                reference_simd_index_execution_stats combined{};
                                std::uint64_t subcore_lo = core_lo;
                                while (subcore_lo < core_hi) {
                                    const std::uint64_t subcore_hi =
                                        subcore_lo + std::min(maximum_core_bp,
                                                              core_hi - subcore_lo);
                                    const std::uint64_t subcollect_lo =
                                        subcore_lo >= halo ? subcore_lo - halo : 0u;
                                    const std::uint64_t right_halo =
                                        std::min(halo, target_length - subcore_hi);
                                    const std::uint64_t subcollect_hi =
                                        subcore_hi + right_halo;
                                    if (subcollect_hi < subcollect_lo ||
                                        subcollect_hi - subcollect_lo > local_position_capacity) {
                                        throw std::invalid_argument(
                                            "reference fragment owner range exceeds its record");
                                    }
                                    struct canonical_event_collector {
                                        std::vector<canonical_minimizer_event>* output = nullptr;
                                        std::uint64_t emit_base = 0u;
                                        std::uint64_t core_lo = 0u;
                                        std::uint64_t core_hi = 0u;

                                        /// Keep reference-kernel minimizers within this subcore and
                                        /// convert their positions to global coordinates.
                                        void collect(const p_minimizer& minimizer) {
                                            const std::uint64_t position =
                                                emit_base +
                                                static_cast<std::uint64_t>(minimizer.last_pos());
                                            if (position < core_lo || position >= core_hi) {
                                                return;
                                            }
                                            output->push_back(canonical_minimizer_event{
                                                .hash = minimizer.kmer(),
                                                .encoded_position =
                                                    (position << 1u) |
                                                    static_cast<std::uint64_t>(minimizer.strand()),
                                            });
                                        }
                                    } direct_collector{
                                        .output = &run,
                                        .emit_base = subcollect_lo,
                                        .core_lo = subcore_lo,
                                        .core_hi = subcore_hi,
                                    };
                                    combined.merge(
                                        collect_reference_simd_index_range_or_throw<p_minimizer>(
                                            direct_collector,
                                            sequence_,
                                            alphabet_code_,
                                            static_cast<sequence_size_t>(subcollect_lo),
                                            static_cast<sequence_size_t>(subcollect_hi),
                                            window_size_,
                                            kmer_size_,
                                            algorithm_identity_.packed_storage_bits,
                                            kernels_,
                                            "minimizer fragment generation replay"));
                                    subcore_lo = subcore_hi;
                                }
                                return combined;
                            });
                        reference_simd_stats_.merge(stats);
                        reference_simd_stats_present_ = true;
                        route_stats_saturated_ =
                            route_stats_saturated_ || reference_simd_stats_.saturated;
                    } else if (algorithm_identity_.effective_algorithm ==
                               bioseq::simd::minimizer_algorithm::two_stack_nthash) {
                        const auto stats = collect_exact_simd_index_range_or_throw(
                            collector,
                            sequence_,
                            alphabet_code_,
                            static_cast<sequence_size_t>(collect_lo),
                            static_cast<sequence_size_t>(collect_hi),
                            window_size_,
                            kmer_size_,
                            0u,
                            kernels_,
                            "minimizer fragment generation replay");
                        exact_simd_stats_.merge(stats);
                        exact_simd_stats_present_ = true;
                        route_stats_saturated_ =
                            route_stats_saturated_ || exact_simd_stats_.saturated;
                    } else {
                        return std::unexpected(
                            minimizer_generation_encoding_error::storage_contract_mismatch);
                    }
                } catch (const exact_simd_index_runtime_error&) {
                    return std::unexpected(
                        minimizer_generation_encoding_error::storage_contract_mismatch);
                } catch (const std::invalid_argument&) {
                    return std::unexpected(
                        minimizer_generation_encoding_error::storage_contract_mismatch);
                }
            } else {
                return std::unexpected(
                    minimizer_generation_encoding_error::storage_contract_mismatch);
            }
            const auto& sketches = collector.extract_sketches_data();
            run.reserve(sketches.size());
            for (const minimap_sketch_data& minimizer : sketches) {
                const std::uint64_t position =
                    emit_base + static_cast<std::uint64_t>(minimizer.last_pos());
                if (position < core_lo || position >= core_hi) {
                    continue;
                }
                run.push_back(canonical_minimizer_event{
                    .hash = minimizer.kmer(),
                    .encoded_position =
                        (position << 1u) | static_cast<std::uint64_t>(minimizer.strand()),
                });
            }
        }
        canonicalize_core_run_(run);
        // Check cancellatihn after this core's collection and sorting finish.
        cancellation_checkpoint();
        return run;
    }

  public:
    /// Select the sequence, backend, kernel table, and minimizer parameters.
    /// Borrowed sequence  storage, kernel tables, and cancellation context must
    /// stay alive through all producer scans.
    minimizer_fragment_generation_replay_producer(
        sequence_type sequence,
        minimizer_sketch_backend backend,
        const bioseq::minimizers::minimizer_kernels* kernels,
        minimizer_generation_algorithm_identity algorithm_identity,
        std::uint16_t window_size,
        std::uint16_t kmer_size,
        std::uint8_t alphabet_code,
        std::size_t gpu_run_buffer_bytes = 0u,
        cooperative_cancellation_checkpoint cancellation_checkpoint = {}) noexcept
        : sequence_(sequence), backend_(backend), kernels_(kernels),
          algorithm_identity_(algorithm_identity), alphabet_code_(alphabet_code),
          window_size_(window_size), kmer_size_(kmer_size),
          gpu_run_buffer_bytes_(gpu_run_buffer_bytes),
          cancellation_checkpoint_(cancellation_checkpoint) {}

    /// Set the storage layout before any scan. Rebinding a different layout,
    /// using an unsupported layout, or binding after a scan invalidates the stats.
    void bind_generation_layout(minimizer_generation_layout layout) noexcept {
        const std::uint64_t required =
            layout == minimizer_generation_layout::grouped_soa_segment_local ? 3u
            : is_supported_minimizer_generation_layout(layout)               ? 2u
                                                                             : 0u;
        if (required == 0u || scan_count_ != 0u ||
            (generation_layout_ != minimizer_generation_layout::invalid &&
             generation_layout_ != layout)) {
            route_stats_saturated_ = true;
            return;
        }
        generation_layout_ = layout;
    }

    /// Return the persisted minimizer scheme selected by this producer backend.
    [[nodiscard]] minimizer_scheme_identity scheme_identity() const noexcept {
        return minimizer_scheme_identity_for_backend(backend_);
    }

    /// Return the persisted producer identity selected by this backend.
    [[nodiscard]] minimizer_producer_identity producer_backend_identity() const noexcept {
        return minimizer_producer_backend_identity_for_backend(backend_);
    }

    /// Return the algorithm and record format selected before scanning
    [[nodiscard]] minimizer_generation_algorithm_identity algorithm_identity() const noexcept {
        return algorithm_identity_;
    }

    /// Rescan the, sequence for a chosen persisted segment width.
    /// Emit segments in position order, with each segment's events sorted by
    /// hash  and position/strand. Changing persisted_span records a geometry
    /// retry. Stop and propagate a receiver error when it rejects an event.
    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    scan_sorted_events(std::uint64_t persisted_span, canonical_minimizer_event_receiver receiver) {
        constexpr std::uint64_t fragment_bp = 32'768u;
        increment_route_counter_(scan_count_);
        if (has_last_scan_span_ && persisted_span != last_scan_span_) {
            increment_route_counter_(geometry_retry_count_);
        }
        last_scan_span_ = persisted_span;
        has_last_scan_span_ = true;
        if (persisted_span == 0 || fragment_bp == 0 || window_size_ == 0 || kmer_size_ == 0 ||
            static_cast<std::uint32_t>(window_size_) + static_cast<std::uint32_t>(kmer_size_) <
                2u) {
            return std::unexpected(minimizer_generation_encoding_error::segment_span);
        }
        if (backend_ != minimizer_sketch_backend::scalar &&
            backend_ != minimizer_sketch_backend::exact_simd &&
            backend_ != minimizer_sketch_backend::bounded_nthash_simd &&
            backend_ != minimizer_sketch_backend::vulkan_segment) {
            return std::unexpected(minimizer_generation_encoding_error::storage_contract_mismatch);
        }
        if (!minimizer_generation_kernel_binding_is_valid(
                backend_, algorithm_identity_, kernels_)) {
            return std::unexpected(minimizer_generation_encoding_error::storage_contract_mismatch);
        }
        const std::uint64_t target_length = sequence_.bp_size();
        cancellation_checkpoint();

        // When persisted segments match physical fragments, batch Vulkan
        // uploads across adjacent fragments. Keep each fragment's sorted
        // run separate. The record type only needs to hold one fragment
        // and its context, even when the batch spans many fragments.
        if (backend_ == minimizer_sketch_backend::vulkan_segment
            && persisted_span == fragment_bp && target_length != 0u) {
            const std::uint64_t halo =
                static_cast<std::uint64_t>(window_size_) + kmer_size_ - 2u;
            const std::uint64_t aggregate_halo =
                bioseq::common::saturating_mul(halo, 2u);
            const std::uint64_t fragment_count_u64 =
                target_length / fragment_bp
                + static_cast<std::uint64_t>(target_length % fragment_bp != 0u);
            if (!std::in_range<std::size_t>(fragment_count_u64)
                || !std::in_range<std::size_t>(fragment_bp)
                || !std::in_range<std::size_t>(aggregate_halo)) {
                return std::unexpected(
                    minimizer_generation_encoding_error::arithmetic_overflow);
            }
            const std::size_t fragments_per_batch =
                gpu_minimizer_fragments_per_batch(
                    static_cast<std::size_t>(fragment_count_u64),
                    static_cast<std::size_t>(fragment_bp),
                    static_cast<std::size_t>(aggregate_halo),
                    gpu_run_buffer_bytes_);
            if (!std::in_range<std::uint64_t>(fragments_per_batch)
                || fragments_per_batch == 0u
                || static_cast<std::uint64_t>(fragments_per_batch)
                    > (std::numeric_limits<std::uint64_t>::max)() / fragment_bp) {
                return std::unexpected(
                    minimizer_generation_encoding_error::arithmetic_overflow);
            }
            const std::uint64_t batch_core_capacity =
                static_cast<std::uint64_t>(fragments_per_batch) * fragment_bp;
            const std::uint64_t maximum_window_bp = std::min(
                target_length,
                bioseq::common::saturating_add(fragment_bp, aggregate_halo));
            return bioseq::gpu::vulkan::
                with_vulkan_segment_pair_record<p_alphabet>(
                    window_size_,
                    kmer_size_,
                    maximum_window_bp,
                    [this,
                     target_length,
                     fragment_bp,
                     batch_core_capacity,
                     &receiver]<class p_minimizer_type>()
                        -> std::expected<void,
                                         minimizer_generation_encoding_error> {
                        std::uint64_t batch_lo = 0u;
                        while (batch_lo < target_length) {
                            const std::uint64_t batch_width =
                                std::min(batch_core_capacity,
                                         target_length - batch_lo);
                            if (batch_width == 0u) {
                                return std::unexpected(
                                    minimizer_generation_encoding_error::
                                        arithmetic_overflow);
                            }
                            const std::uint64_t batch_hi = batch_lo + batch_width;
                            auto runs = this->template collect_vulkan_segment_runs_<
                                p_minimizer_type>(batch_lo,
                                                  batch_hi,
                                                  fragment_bp,
                                                  fragment_bp);
                            if (!runs) {
                                return std::unexpected(runs.error());
                            }
                            std::uint64_t buffered_events = 0u;
                            for (const auto& run : *runs) {
                                if (buffered_events
                                    > (std::numeric_limits<std::uint64_t>::max)()
                                        - run.events.size()) {
                                    return std::unexpected(
                                        minimizer_generation_encoding_error::
                                            arithmetic_overflow);
                                }
                                buffered_events += run.events.size();
                            }
                            peak_buffered_events_ =
                                std::max(peak_buffered_events_, buffered_events);
                            peak_buffered_core_bp_ =
                                std::max(peak_buffered_core_bp_, batch_width);
                            peak_active_fragment_runs_ =
                                std::max<std::uint64_t>(
                                    peak_active_fragment_runs_, runs->size());

                            // Runs are sorted yithin each fragment and stored in fragment
                            // order. Emit them separately to preserve segment boundaries.
                            for (const auto& run : *runs) {
                                for (const canonical_minimizer_event& event :
                                     run.events) {
                                    if (auto accepted = receiver(event); !accepted) {
                                        return accepted;
                                    }
                                }
                                this->cancellation_checkpoint();
                            }
                            batch_lo = batch_hi;
                        }
                        return {};
                    });
        }

        // Other persisted widths can split physical fragments. Batch
        // complete segments, keeping a  separate upload and context range
        // for each segment/fragment intersection.
        if (backend_ == minimizer_sketch_backend::vulkan_segment
            && persisted_span != fragment_bp && target_length != 0u) {
            const std::uint64_t halo =
                static_cast<std::uint64_t>(window_size_) + kmer_size_ - 2u;
            const std::uint64_t aggregate_halo =
                bioseq::common::saturating_mul(halo, 2u);
            const std::uint64_t segment_count_u64 =
                target_length / persisted_span
                + static_cast<std::uint64_t>(target_length % persisted_span != 0u);
            // Bound how many physical fragments a segment can intersect,
            // allowing for a start that falls inside a fragment.
            const std::uint64_t cores_per_segment_bound_u64 =
                persisted_span / fragment_bp
                + (persisted_span % fragment_bp == 0u ? 1u : 2u);
            if (std::in_range<std::size_t>(segment_count_u64)
                && std::in_range<std::size_t>(cores_per_segment_bound_u64)
                && cores_per_segment_bound_u64 != 0u
                && segment_count_u64
                    <= (std::numeric_limits<std::size_t>::max)()
                        / static_cast<std::size_t>(
                            cores_per_segment_bound_u64)
                && std::in_range<std::size_t>(fragment_bp)
                && std::in_range<std::size_t>(aggregate_halo)) {
                const std::size_t maximum_core_count =
                    static_cast<std::size_t>(segment_count_u64)
                    * static_cast<std::size_t>(
                        cores_per_segment_bound_u64);
                const std::size_t core_capacity =
                    gpu_minimizer_fragments_per_batch(
                        maximum_core_count,
                        static_cast<std::size_t>(fragment_bp),
                        static_cast<std::size_t>(aggregate_halo),
                        gpu_run_buffer_bytes_);
                const std::size_t segment_capacity =
                    core_capacity
                    / static_cast<std::size_t>(
                        cores_per_segment_bound_u64);
                if (segment_capacity != 0u) {
                    if (!std::in_range<std::uint64_t>(segment_capacity)
                        || static_cast<std::uint64_t>(segment_capacity)
                            > (std::numeric_limits<std::uint64_t>::max)()
                                / persisted_span) {
                        return std::unexpected(
                            minimizer_generation_encoding_error::
                                arithmetic_overflow);
                    }
                    const std::uint64_t batch_segment_capacity =
                        static_cast<std::uint64_t>(segment_capacity)
                        * persisted_span;
                    const std::uint64_t maximum_window_bp = std::min(
                        target_length,
                        bioseq::common::saturating_add(fragment_bp,
                            aggregate_halo));
                    return bioseq::gpu::vulkan::
                        with_vulkan_segment_pair_record<p_alphabet>(
                            window_size_,
                            kmer_size_,
                            maximum_window_bp,
                            [this,
                             target_length,
                             persisted_span,
                             fragment_bp,
                             batch_segment_capacity,
                             &receiver]<class p_minimizer_type>()
                                -> std::expected<
                                    void,
                                    minimizer_generation_encoding_error> {
                                std::uint64_t batch_lo = 0u;
                                while (batch_lo < target_length) {
                                    const std::uint64_t batch_width = std::min(
                                        batch_segment_capacity,
                                        target_length - batch_lo);
                                    if (batch_width == 0u) {
                                        return std::unexpected(
                                            minimizer_generation_encoding_error::
                                                arithmetic_overflow);
                                    }
                                    const std::uint64_t batch_hi =
                                        batch_lo + batch_width;
                                    auto core_runs = this->template
                                        collect_vulkan_segment_runs_<
                                            p_minimizer_type>(
                                                batch_lo,
                                                batch_hi,
                                                fragment_bp,
                                                persisted_span);
                                    if (!core_runs) {
                                        return std::unexpected(core_runs.error());
                                    }
                                    const std::uint64_t batch_segment_count_u64 =
                                        batch_width / persisted_span
                                        + static_cast<std::uint64_t>(
                                            batch_width % persisted_span != 0u);
                                    if (!std::in_range<std::size_t>(
                                            batch_segment_count_u64)) {
                                        return std::unexpected(
                                            minimizer_generation_encoding_error::
                                                arithmetic_overflow);
                                    }
                                    std::vector<std::vector<std::vector<
                                        canonical_minimizer_event>>> segment_runs(
                                            static_cast<std::size_t>(
                                                batch_segment_count_u64));
                                    std::uint64_t buffered_events = 0u;
                                    for (auto& core_run : *core_runs) {
                                        if (core_run.output_segment_lo < batch_lo) {
                                            return std::unexpected(
                                                minimizer_generation_encoding_error::
                                                    arithmetic_overflow);
                                        }
                                        const std::uint64_t relative_segment_lo =
                                            core_run.output_segment_lo - batch_lo;
                                        if (relative_segment_lo % persisted_span
                                            != 0u) {
                                            return std::unexpected(
                                                minimizer_generation_encoding_error::
                                                    arithmetic_overflow);
                                        }
                                        const std::uint64_t segment_slot_u64 =
                                            relative_segment_lo / persisted_span;
                                        if (!std::in_range<std::size_t>(
                                                segment_slot_u64)
                                            || segment_slot_u64
                                                >= batch_segment_count_u64
                                            || buffered_events
                                                > (std::numeric_limits<
                                                       std::uint64_t>::max)()
                                                    - core_run.events.size()) {
                                            return std::unexpected(
                                                minimizer_generation_encoding_error::
                                                    arithmetic_overflow);
                                        }
                                        buffered_events += core_run.events.size();
                                        segment_runs[static_cast<std::size_t>(
                                            segment_slot_u64)]
                                            .push_back(
                                                std::move(core_run.events));
                                    }
                                    peak_buffered_events_ = std::max(
                                        peak_buffered_events_, buffered_events);
                                    peak_buffered_core_bp_ = std::max(
                                        peak_buffered_core_bp_, batch_width);
                                    peak_active_fragment_runs_ =
                                        std::max<std::uint64_t>(
                                            peak_active_fragment_runs_,
                                            core_runs->size());
                                    for (const auto& runs : segment_runs) {
                                        auto emitted = emit_sorted_core_runs_(
                                            runs, receiver);
                                        if (!emitted) {
                                            return emitted;
                                        }
                                        this->cancellation_checkpoint();
                                    }
                                    batch_lo = batch_hi;
                                }
                                return {};
                            });
                }
            }
        }

        std::uint64_t segment_lo = 0;
        while (segment_lo < target_length) {
            const std::uint64_t segment_width =
                std::min(persisted_span, target_length - segment_lo);
            const std::uint64_t segment_hi = segment_lo + segment_width;
            std::vector<std::vector<canonical_minimizer_event>> runs;
            std::uint64_t buffered_events = 0;
            if (backend_ == minimizer_sketch_backend::vulkan_segment) {
                const std::uint64_t halo =
                    static_cast<std::uint64_t>(window_size_) + kmer_size_ - 2u;
                const std::uint64_t maximum_window_bp = std::min(
                    target_length,
                    bioseq::common::saturating_add(
                        fragment_bp,
                        bioseq::common::saturating_mul(halo, 2u)));
                auto batched_runs = bioseq::gpu::vulkan::
                    with_vulkan_segment_pair_record<p_alphabet>(
                        window_size_,
                        kmer_size_,
                        maximum_window_bp,
                        [this,
                         segment_lo,
                         segment_hi,
                         fragment_bp,
                         persisted_span]
                        <class p_minimizer_type>() {
                            return this->template collect_vulkan_segment_runs_<
                                p_minimizer_type>(
                                    segment_lo,
                                    segment_hi,
                                    fragment_bp,
                                    persisted_span);
                        });
                if (!batched_runs) {
                    return std::unexpected(batched_runs.error());
                }
                for (auto& run : *batched_runs) {
                    if (buffered_events
                        > (std::numeric_limits<std::uint64_t>::max)()
                            - run.events.size()) {
                        return std::unexpected(
                            minimizer_generation_encoding_error::arithmetic_overflow);
                    }
                    buffered_events += run.events.size();
                    runs.push_back(std::move(run.events));
                }
            } else {
                std::uint64_t fragment_lo =
                    (segment_lo / fragment_bp) * fragment_bp;
                while (fragment_lo < segment_hi) {
                    const std::uint64_t fragment_hi =
                        std::min(target_length, fragment_lo + fragment_bp);
                    const std::uint64_t core_lo = std::max(segment_lo, fragment_lo);
                    const std::uint64_t core_hi = std::min(segment_hi, fragment_hi);
                    if (core_lo < core_hi) {
                        auto run = collect_core_run(core_lo, core_hi);
                        if (!run) {
                            return std::unexpected(run.error());
                        }
                        if (buffered_events
                            > (std::numeric_limits<std::uint64_t>::max)()
                                - run->size()) {
                            return std::unexpected(
                                minimizer_generation_encoding_error::arithmetic_overflow);
                        }
                        buffered_events += run->size();
                        if (!run->empty()) {
                            runs.push_back(std::move(*run));
                        }
                    }
                    if (fragment_hi <= fragment_lo) {
                        return std::unexpected(
                            minimizer_generation_encoding_error::arithmetic_overflow);
                    }
                    fragment_lo = fragment_hi;
                }
            }
            peak_buffered_events_ = std::max(peak_buffered_events_, buffered_events);
            peak_buffered_core_bp_ = std::max(peak_buffered_core_bp_, segment_width);
            peak_active_fragment_runs_ =
                std::max<std::uint64_t>(peak_active_fragment_runs_, runs.size());

            auto emitted = emit_sorted_core_runs_(runs, receiver);
            if (!emitted) {
                return emitted;
            }
            segment_lo = segment_hi;
            cancellation_checkpoint();
        }
        cancellation_checkpoint();
        return {};
    }

    /// Return the number of attempted producer scans, including geometry retries.
    [[nodiscard]] std::uint64_t scan_count() const noexcept {
        return scan_count_;
    }
    /// Return the largest number of canonical events buffered for a collection batch.
    [[nodiscard]] std::uint64_t peak_buffered_events() const noexcept {
        return peak_buffered_events_;
    }
    /// Return the largest aggregate core width buffered in base pairs.
    [[nodiscard]] std::uint64_t peak_buffered_core_bp() const noexcept {
        return peak_buffered_core_bp_;
    }
    /// Return the largest number of fragment runs buffered concurrently.
    [[nodiscard]] std::uint64_t peak_active_fragment_runs() const noexcept {
        return peak_active_fragment_runs_;
    }
    /// Return the largest bounded ntHash interval copied into packed storage, in base pairs.
    [[nodiscard]] std::uint64_t peak_bounded_materialized_window_bp() const noexcept {
        return peak_bounded_materialized_window_bp_;
    }
    /// Whether a two-stack ntHash core supplied execution counters.
    [[nodiscard]] bool exact_simd_stats_present() const noexcept {
        return exact_simd_stats_present_;
    }
    /// Borrow the combined two-stack ntHash execution counters.
    [[nodiscard]] const exact_simd_index_execution_stats& exact_simd_stats() const noexcept {
        return exact_simd_stats_;
    }
    /// Whether a bounded ntHash call supplied execution counters.
    [[nodiscard]] bool packed_bounded_stats_present() const noexcept {
        return packed_bounded_stats_present_;
    }
    /// Return the most recently merged bounded ntHash status.
    [[nodiscard]] packed_bounded_nthash_index_status packed_bounded_status() const noexcept {
        return packed_bounded_status_;
    }
    /// Borrow the  combined bounded ntHash execution counters.
    [[nodiscard]] const packed_bounded_nthash_index_execution_stats&
    packed_bounded_stats() const noexcept {
        return packed_bounded_stats_;
    }
    /// Copy backend identities and execution counters, saving a private snapshot
    /// so publication checks can detect later changes.
    [[nodiscard]] minimizer_fragment_generation_execution_receipt execution_stats() const noexcept {
        minimizer_fragment_generation_execution_receipt stats;
        stats.selected_backend = backend_;
        stats.minimizer_scheme = scheme_identity();
        stats.algorithm_identity = algorithm_identity_;
        stats.target_length = sequence_.bp_size();
        stats.window_size = window_size_;
        stats.kmer_size = kmer_size_;
        stats.producer_scan_count = scan_count_;
        stats.geometry_retry_count = geometry_retry_count_;
        stats.generation_layout = generation_layout_;
        stats.short_core_runs = short_core_runs_;
        stats.scalar_core_runs = scalar_core_runs_;
        stats.exact_simd_core_runs = exact_simd_core_runs_;
        stats.bounded_nthash_core_runs = bounded_nthash_core_runs_;
        stats.vulkan_core_runs = vulkan_core_runs_;
        stats.exact_simd_stats = exact_simd_stats_;
        stats.exact_simd_stats_present = exact_simd_stats_present_;
        stats.reference_simd_stats = reference_simd_stats_;
        stats.reference_simd_stats_present = reference_simd_stats_present_;
        stats.packed_bounded_status = packed_bounded_status_;
        stats.packed_bounded_stats = packed_bounded_stats_;
        stats.packed_bounded_stats_present = packed_bounded_stats_present_;
        stats.vulkan_stats = vulkan_stats_;
        stats.backend_bound = true;
        stats.saturated = route_stats_saturated_ || vulkan_stats_.saturated;
        stats.capture_route_snapshot_();
        return stats;
    }

    /// Copy execution stats and attach the completed generation's identity
    /// publication_accepted() rejects mismatched generation metadata or later
    /// changes to, the stats.
    /// @param generation Completed encoded generation.
    /// @return Stats with a valid binding when the generation matches.
    [[nodiscard]] minimizer_fragment_generation_execution_receipt
    publication_execution_stats(
        const encoded_grouped_soa_generation& generation) const noexcept {
        auto stats = execution_stats();
        stats.bind_publication_(generation);
        return stats;
    }
};

/// Build and read fragment minimizer indexes in chunked storag.
/// @tparam p_storage Storage supporting the fragment_chunks index layout.
template <class p_storage> struct minimizers_per_fragment_index {
    static_assert(is_index_chunked_storage_for<p_storage, sequence_index_layout::fragment_chunks>);
    /// Registry layout used for one serialized blob per physical fragment.
    static constexpr sequence_index_layout index_type = sequence_index_layout::fragment_chunks;
    /// Stable registry name inherited from the fragment hash-map representation.
    static constexpr std::string_view index_name = minimizer_hashmap_fragment_index::index_name;
    /// Storage type used by these operations.
    using storage_type = p_storage;

    /// Make a producer  that can rescan the sequence for generation encoding.
    /// @tparam p_alphabet Alphabet of the borrowed sequence.
    template <sequence_alphabet p_alphabet>
    [[nodiscard]] static auto make_generation_replay_producer(
        bioseq::storage::lazy_sequence_view<p_alphabet> sequence,
        minimizer_sketch_backend backend,
        const bioseq::minimizers::minimizer_kernels* kernels,
        minimizer_generation_algorithm_identity algorithm_identity,
        std::uint16_t window_size,
        std::uint16_t kmer_size,
        std::uint8_t alphabet_code = bioseq::minimizers::k_unbound_packed_alphabet_code,
        std::size_t gpu_run_buffer_bytes = 0u,
        cooperative_cancellation_checkpoint cancellation_checkpoint = {}) {
        return minimizer_fragment_generation_replay_producer<p_alphabet>{
            sequence,
            backend,
            kernels,
            algorithm_identity,
            window_size,
            kmer_size,
            alphabet_code,
            gpu_run_buffer_bytes,
            cancellation_checkpoint};
    }

    /// Encode a minimizer generation with the selected backend and send rows to sink.
    /// Check the generated algorithm identity before collection, then require
    /// execution stats and replay counts to match the completed manifest.
    /// A failure can leave rows already delivered to the sink.
    ///
    /// @tparam p_alphabet Alphabet of the borrowed sequence.
    /// @tparam p_sink Sink accepting encoded generation rows.
    template <sequence_alphabet p_alphabet, minimizer_generation_row_sink p_sink>
    [[nodiscard]] static std::expected<encoded_grouped_soa_generation,
                                       minimizer_generation_encoding_error>
    encode_persisted_generation(
        bioseq::storage::lazy_sequence_view<p_alphabet> sequence,
        minimizer_sketch_backend backend,
        const bioseq::minimizers::minimizer_kernels* kernels,
        p_sink& sink,
        const grouped_soa_generation_options& options,
        const grouped_soa_generation_limits& limits = {},
        minimizer_fragment_generation_execution_receipt* execution_stats_out = nullptr,
        std::uint8_t runtime_alphabet_code = bioseq::minimizers::k_unbound_packed_alphabet_code,
        std::size_t gpu_run_buffer_bytes = 0u,
        cooperative_cancellation_checkpoint cancellation_checkpoint = {}) {
        if (execution_stats_out != nullptr) {
            *execution_stats_out = {};
        }
        const auto selected_identity =
            select_minimizer_generation_algorithm_identity<p_alphabet>(
                options.algorithm_identity.requested_algorithm,
                k_minimizer_fragment_algorithm_position_extent_bp,
                options.algorithm_identity.packed_storage_bits,
                options.kmer_size,
                options.window_size);
        const minimizer_producer_identity producer_backend =
            minimizer_producer_backend_identity_for_backend(backend);
        if (!selected_identity || *selected_identity != options.algorithm_identity ||
            !is_valid_minimizer_generation_algorithm_identity(options.algorithm_identity,
                                                              producer_backend)) {
            return std::unexpected(minimizer_generation_encoding_error::storage_contract_mismatch);
        }
        if (!minimizer_generation_kernel_binding_is_valid(
                backend, options.algorithm_identity, kernels)) {
            return std::unexpected(minimizer_generation_encoding_error::storage_contract_mismatch);
        }
        auto producer = make_generation_replay_producer(sequence,
                                                        backend,
                                                        kernels,
                                                        options.algorithm_identity,
                                                        options.window_size,
                                                        options.kmer_size,
                                                        runtime_alphabet_code,
                                                        gpu_run_buffer_bytes,
                                                        cancellation_checkpoint);
        auto encoded = encode_minimizer_generation(producer, sink, options, limits);
        minimizer_fragment_generation_execution_receipt stats = producer.execution_stats();
        const minimizer_producer_identity stats_backend =
            minimizer_producer_backend_identity_for_backend(stats.selected_backend);
        if (encoded && (encoded->manifest.producer_backend_id != stats_backend.id ||
                        encoded->manifest.producer_identity_revision != stats_backend.revision ||
                        encoded->manifest.algorithm_identity != stats.algorithm_identity)) {
            encoded =
                std::unexpected(minimizer_generation_encoding_error::storage_contract_mismatch);
        }
        if (encoded) {
            stats.bind_publication_(*encoded);
        }
        if (encoded &&
            !stats.publication_accepted(make_minimizer_generation_publication_binding(*encoded))) {
            encoded =
                std::unexpected(minimizer_generation_encoding_error::storage_contract_mismatch);
        }
        if (execution_stats_out != nullptr) {
            *execution_stats_out = stats;
        }
        return encoded;
    }

    /// Build one transient blob per 32,768-base fragment using a  registered algorithm.
    /// Check its identity before collection. Write each fragment, including empty
    /// ones, and then the fragment-count manifest. Execution stats are checked
    /// after thede writes, so an error can leave storage modified.
    template <sequence_alphabet p_alphabet>
    [[nodiscard]] static std::expected<minimizer_fragment_generation_execution_receipt,
                                       minimizer_generation_encoding_error>
    build_registered(
        p_storage& storage,
        bioseq::storage::lazy_sequence_view<p_alphabet> sequence,
        minimizer_sketch_backend backend,
        const bioseq::minimizers::minimizer_kernels* kernels,
        minimizer_generation_algorithm_identity algorithm_identity,
        std::uint16_t window_size,
        std::uint16_t kmer_size,
        std::uint8_t alphabet_code) {
        constexpr std::uint64_t fragment_bp = 32'768u;
        const auto selected_identity =
            select_minimizer_generation_algorithm_identity<p_alphabet>(
                algorithm_identity.requested_algorithm,
                k_minimizer_fragment_algorithm_position_extent_bp,
                algorithm_identity.packed_storage_bits,
                kmer_size,
                window_size);
        const minimizer_producer_identity producer_backend =
            minimizer_producer_backend_identity_for_backend(backend);
        if (!selected_identity ||
            *selected_identity != algorithm_identity ||
            algorithm_identity.alphabet_id != alphabet_code ||
            !is_valid_minimizer_generation_algorithm_identity(algorithm_identity,
                                                               producer_backend) ||
            !minimizer_generation_kernel_binding_is_valid(backend,
                                                           algorithm_identity,
                                                           kernels)) {
            return std::unexpected(
                minimizer_generation_encoding_error::storage_contract_mismatch);
        }

        const std::uint64_t target_length = sequence.bp_size();
        if (target_length == 0u) {
            return std::unexpected(minimizer_generation_encoding_error::position_range);
        }
        const std::uint64_t fragment_count =
            target_length / fragment_bp + (target_length % fragment_bp != 0u ? 1u : 0u);
        if (fragment_count >
                static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)()) ||
            fragment_count >
                static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())) {
            return std::unexpected(minimizer_generation_encoding_error::segment_count);
        }

        auto producer = make_generation_replay_producer(sequence,
                                                        backend,
                                                        kernels,
                                                        algorithm_identity,
                                                        window_size,
                                                        kmer_size,
                                                        alphabet_code);
        struct fragment_writer_state {
            p_storage* storage = nullptr;
            std::uint64_t target_length = 0u;
            std::uint64_t fragment_count = 0u;
            std::uint64_t next_fragment = 0u;
            std::uint64_t fragment_span = 0u;
            std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs;
            std::vector<char> blob;

            /** Serialize and write the next complete fragment, then clear its pair buffer. */
            [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
            flush_one() {
                if (storage == nullptr || next_fragment >= fragment_count ||
                    next_fragment >
                        (std::numeric_limits<index_sub_key_t>::max)()) {
                    return std::unexpected(minimizer_generation_encoding_error::segment_count);
                }
                const std::uint64_t fragment_lo = next_fragment * fragment_span;
                if (fragment_lo > target_length) {
                    return std::unexpected(minimizer_generation_encoding_error::position_range);
                }
                const std::uint64_t fragment_hi =
                    fragment_lo + std::min(fragment_span, target_length - fragment_lo);
                serialize_sorted_pairs_to_fragment_blob(
                    std::span<const std::pair<std::uint64_t, std::uint64_t>>{pairs},
                    static_cast<sequence_size_t>(fragment_hi - fragment_lo),
                    blob);
                const index_sub_key_t key = static_cast<index_sub_key_t>(next_fragment);
                const std::size_t bytes = blob.size();
                storage->resize(key, bytes);
                storage->template write_to_each<char>(
                    key,
                    [this, bytes](std::size_t chunk_no,
                                  char* output,
                                  std::size_t capacity) {
                        if (chunk_no != 0u || capacity < bytes) {
                            storage->index_error(
                                "registered transient fragment blob has invalid chunk geometry");
                        }
                        if (bytes != 0u) {
                            std::memcpy(output, blob.data(), bytes);
                        }
                        return chunk_no;
                    });
                pairs.clear();
                ++next_fragment;
                return {};
            }

            /** Place a global event in its fragment, writing earlier fragments as needed. */
            [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
            accept(const canonical_minimizer_event& event) {
                const std::uint64_t position = event.encoded_position >> 1u;
                if (position >= target_length) {
                    return std::unexpected(minimizer_generation_encoding_error::position_range);
                }
                const std::uint64_t fragment = position / fragment_span;
                while (next_fragment < fragment) {
                    if (auto flushed = flush_one(); !flushed) {
                        return flushed;
                    }
                }
                if (fragment != next_fragment) {
                    return std::unexpected(minimizer_generation_encoding_error::row_order);
                }
                const std::uint64_t local_position = position - fragment * fragment_span;
                pairs.emplace_back(event.hash,
                                   (local_position << 1u) | (event.encoded_position & 1u));
                return {};
            }
        } writer{
            .storage = std::addressof(storage),
            .target_length = target_length,
            .fragment_count = fragment_count,
            .fragment_span = fragment_bp,
        };
        const auto scanned = producer.scan_sorted_events(
            fragment_bp,
            canonical_minimizer_event_receiver{
                .context = std::addressof(writer),
                .emit_fn = +[](void* context, const canonical_minimizer_event& event) {
                    return static_cast<fragment_writer_state*>(context)->accept(event);
                },
            });
        if (!scanned) {
            return std::unexpected(scanned.error());
        }
        while (writer.next_fragment < writer.fragment_count) {
            if (auto flushed = writer.flush_one(); !flushed) {
                return std::unexpected(flushed.error());
            }
        }
        const std::int64_t manifest = static_cast<std::int64_t>(fragment_count);
        storage.resize(k_directory_sub_key, sizeof(manifest));
        storage.template write_to_each<std::int64_t>(
            k_directory_sub_key,
            [manifest](std::size_t chunk_no, std::int64_t* output, std::size_t capacity) {
                if (chunk_no != 0u || capacity < 1u) {
                    throw std::logic_error{
                        "registered transient fragment manifest has invalid chunk geometry"};
                }
                output[0] = manifest;
                return chunk_no;
            });
        minimizer_fragment_generation_execution_receipt receipt = producer.execution_stats();
        if (!receipt.direct_build_accepted() || receipt.algorithm_identity != algorithm_identity ||
            receipt.selected_backend != backend || receipt.target_length != target_length) {
            return std::unexpected(
                minimizer_generation_encoding_error::storage_contract_mismatch);
        }
        return receipt;
    }

    /// Build one blob per 32,768-base fragment with k=15 and w=10.
    /// Store fragment-local positions and their strand bits. Write the
    /// fragment-count manifest after all blobs. Collection or storage failures
    /// can leave a partially written  index
    ///
    /// @tparam p_alphabet Alphabet of the borrowed sequence.
    template <sequence_alphabet p_alphabet>
    static void
    build(p_storage& storage,
          bioseq::storage::lazy_sequence_view<p_alphabet> seq,
          minimizer_sketch_backend backend,
          const bioseq::minimizers::minimizer_kernels* kernels,
          minimizer_fragment_generation_execution_receipt* execution_stats_out = nullptr,
          std::uint8_t alphabet_code = bioseq::minimizers::k_unbound_packed_alphabet_code) {
        constexpr std::size_t w = 10, k = 15; // Fixed minimizer parameters for this compatibility build.
        constexpr std::size_t frag_bp = 32'768u;
        constexpr std::size_t k_context_span = w + k; // Context collected on both sides of each core.
        static_assert(
            frag_bp < (std::size_t{1} << 28),
            "fragment-core-local positions must fit the 28-bit minimap_sketch_data field");
        const sequence_size_t total_bp = seq.bp_size();
        const std::size_t nfrag = static_cast<std::size_t>((total_bp + frag_bp - 1) / frag_bp);
        if (backend == minimizer_sketch_backend::exact_simd &&
            (kernels == nullptr || kernels->two_stack_nthash == nullptr)) {
            throw exact_simd_index_runtime_error{exact_simd_index_status::invalid_state,
                                                 "minimizers_per_fragment build"};
        }
        if (backend == minimizer_sketch_backend::bounded_nthash_simd &&
            (kernels == nullptr || kernels->bounded_nthash == nullptr)) {
            throw packed_bounded_nthash_index_runtime_error{
                packed_bounded_nthash_index_status::invalid_state, "minimizers_per_fragment build"};
        }
        if (execution_stats_out != nullptr) {
            *execution_stats_out = {};
            execution_stats_out->selected_backend = backend;
            execution_stats_out->minimizer_scheme = minimizer_scheme_identity_for_backend(backend);
            execution_stats_out->target_length = total_bp;
            execution_stats_out->window_size = w;
            execution_stats_out->kmer_size = k;
            execution_stats_out->producer_scan_count = 1u;
            execution_stats_out->backend_bound = true;
        }

        // Collect adjacent fragments in one GPU window to share submission
        // costs. Split the sorted global pairs back, into fragment-local blobs.
        if (backend == minimizer_sketch_backend::vulkan_segment) {
            const std::size_t fragments_per_batch = gpu_minimizer_fragments_per_batch(
                nfrag, frag_bp, 2u * k_context_span, storage.run_buffer_bytes());
            std::vector<std::vector<std::pair<std::uint64_t, std::uint64_t>>> per_fragment(
                fragments_per_batch);
            std::vector<char> blob;
            // Replace one fragment chunk with its complete serialized blob.
            const auto write_fragment_blob = [&storage](std::size_t fragment_no,
                                                        const std::vector<char>& fragment_blob) {
                const std::size_t bytes = fragment_blob.size();
                storage.resize(static_cast<index_sub_key_t>(fragment_no), bytes);
                storage.template write_to_each<char>(
                    static_cast<index_sub_key_t>(fragment_no),
                    [&fragment_blob,
                     bytes](std::size_t chunk_no, char* buffer, std::size_t capacity_units) {
                        DEBUG_ASSERT(chunk_no == 0u && capacity_units >= bytes,
                                     "one fragment index fits one chunk");
                        if (bytes != 0u) {
                            std::memcpy(buffer, fragment_blob.data(), bytes);
                        }
                        return chunk_no;
                    });
            };

            for (std::size_t batch_lo_frag = 0u; batch_lo_frag < nfrag;
                 batch_lo_frag += fragments_per_batch) {
                const std::size_t batch_hi_frag =
                    std::min(nfrag, batch_lo_frag + fragments_per_batch);
                const sequence_size_t core_lo =
                    static_cast<sequence_size_t>(batch_lo_frag) * frag_bp;
                const sequence_size_t core_hi =
                    std::min(static_cast<sequence_size_t>(batch_hi_frag) * frag_bp, total_bp);
                const sequence_size_t coll_lo =
                    core_lo >= k_context_span ? core_lo - k_context_span : 0u;
                const sequence_size_t coll_hi =
                    std::min(core_hi + static_cast<sequence_size_t>(k_context_span), total_bp);
                bioseq::gpu::vulkan::vulkan_segment_pair_execution_stats stats{};
                auto batch_pairs =
                    bioseq::gpu::vulkan::collect_sorted_minimizer_pairs_vulkan_segment_or_throw(
                        seq,
                        coll_lo,
                        coll_hi,
                        coll_lo,
                        core_lo,
                        core_hi,
                        w,
                        k,
                        execution_stats_out != nullptr ? &stats : nullptr);
                if (execution_stats_out != nullptr) {
                    ++execution_stats_out->vulkan_core_runs;
                    execution_stats_out->vulkan_stats.merge(stats);
                    execution_stats_out->saturated = execution_stats_out->saturated ||
                                                     execution_stats_out->vulkan_stats.saturated;
                }
                for (auto& fragment_pairs : per_fragment) {
                    fragment_pairs.clear();
                }
                for (const auto& pair : batch_pairs.pairs) {
                    const sequence_size_t global_pos =
                        static_cast<sequence_size_t>(pair.second >> 1u);
                    const std::size_t fragment_no = static_cast<std::size_t>(global_pos / frag_bp);
                    if (fragment_no < batch_lo_frag || fragment_no >= batch_hi_frag) {
                        continue;
                    }
                    const sequence_size_t fragment_lo =
                        static_cast<sequence_size_t>(fragment_no) * frag_bp;
                    per_fragment[fragment_no - batch_lo_frag].emplace_back(
                        pair.first,
                        (static_cast<std::uint64_t>(global_pos - fragment_lo) << 1u) |
                            (pair.second & 1u));
                }
                for (std::size_t fragment_no = batch_lo_frag; fragment_no < batch_hi_frag;
                     ++fragment_no) {
                    const sequence_size_t fragment_lo =
                        static_cast<sequence_size_t>(fragment_no) * frag_bp;
                    const sequence_size_t fragment_hi =
                        std::min(fragment_lo + static_cast<sequence_size_t>(frag_bp), total_bp);
                    serialize_sorted_pairs_to_fragment_blob(
                        std::span<const std::pair<std::uint64_t, std::uint64_t>>{
                            per_fragment[fragment_no - batch_lo_frag]},
                        fragment_hi - fragment_lo,
                        blob);
                    write_fragment_blob(fragment_no, blob);
                }
            }
            const std::int64_t manifest = static_cast<std::int64_t>(nfrag);
            storage.resize(k_directory_sub_key, sizeof(std::int64_t));
            storage.template write_to_each<std::int64_t>(
                k_directory_sub_key,
                [manifest](std::size_t chunk_no, std::int64_t* manifest_buffer, std::size_t) {
                    manifest_buffer[0] = manifest;
                    return chunk_no;
                });
            return;
        }

        // Collect a core with surrounding context, then serialize its local positions.
        auto build_fragment_blob = [seq,
                                    backend,
                                    kernels,
                                    alphabet_code,
                                    total_bp,
                                    execution_stats_out](std::size_t fragment_no,
                                                         sketch_collector& collector,
                                                         std::vector<minimap_sketch_data>& core,
                                                         std::vector<char>& out_blob) {
            const sequence_size_t core_lo = static_cast<sequence_size_t>(fragment_no) * frag_bp;
            const sequence_size_t core_hi =
                std::min(core_lo + static_cast<sequence_size_t>(frag_bp), total_bp);
            const sequence_size_t coll_lo =
                core_lo >= k_context_span ? core_lo - k_context_span : 0;
            const sequence_size_t coll_hi =
                std::min(core_hi + static_cast<sequence_size_t>(k_context_span), total_bp);
            collector.clear();
            const sequence_size_t emit_base = coll_lo;
            const sequence_size_t collect_bp = coll_hi > emit_base ? coll_hi - emit_base : 0;
            core.clear();
            if (collect_bp >= static_cast<sequence_size_t>(k_context_span)) {
                if (backend == minimizer_sketch_backend::vulkan_segment) {
                    auto pairs =
                        bioseq::gpu::vulkan::collect_sorted_minimizer_pairs_vulkan_segment_or_throw(
                            seq, coll_lo, coll_hi, emit_base, core_lo, core_hi, w, k);
                    core.reserve(pairs.pairs.size());
                    for (const auto& pair : pairs.pairs) {
                        const sequence_size_t global_pos =
                            static_cast<sequence_size_t>(pair.second >> 1u);
                        core.push_back(
                            minimap_sketch_data{pair.first,
                                                static_cast<std::uint64_t>(global_pos - core_lo),
                                                static_cast<std::uint64_t>(pair.second & 1u)});
                    }
                } else {
                    if (backend == minimizer_sketch_backend::exact_simd) {
                        const auto stats = collect_exact_simd_index_range_or_throw(
                            collector,
                            seq,
                            alphabet_code,
                            coll_lo,
                            coll_hi,
                            w,
                            k,
                            0u,
                            kernels,
                            "minimizers_per_fragment build");
                        if (execution_stats_out != nullptr) {
                            ++execution_stats_out->exact_simd_core_runs;
                            execution_stats_out->exact_simd_stats.merge(stats);
                            execution_stats_out->exact_simd_stats_present = true;
                            execution_stats_out->saturated =
                                execution_stats_out->saturated ||
                                execution_stats_out->exact_simd_stats.saturated;
                        }
                    } else if (backend == minimizer_sketch_backend::bounded_nthash_simd) {
                        const auto result = collect_packed_bounded_nthash_index_lazy(seq,
                                                                                     alphabet_code,
                                                                                     coll_lo,
                                                                                     coll_hi,
                                                                                     collect_bp,
                                                                                     w,
                                                                                     k,
                                                                                     kernels,
                                                                                     collector);
                        if (result.status != packed_bounded_nthash_index_status::ok) {
                            throw packed_bounded_nthash_index_runtime_error{
                                result.status, "minimizers_per_fragment build"};
                        }
                        if (execution_stats_out != nullptr) {
                            ++execution_stats_out->bounded_nthash_core_runs;
                            execution_stats_out->packed_bounded_status = result.status;
                            execution_stats_out->packed_bounded_stats.merge(result.stats);
                            execution_stats_out->packed_bounded_stats_present = true;
                            execution_stats_out->saturated =
                                execution_stats_out->saturated ||
                                execution_stats_out->packed_bounded_stats.saturated;
                        }
                    } else if (backend == minimizer_sketch_backend::scalar) {
                        if (execution_stats_out != nullptr) {
                            ++execution_stats_out->scalar_core_runs;
                        }
                        bioseq::simd::collect_minimizers_segment_scalar<minimap_sketch_data>(
                            collector, seq, coll_lo, coll_hi, w, k);
                    } else {
                        index_corruption_panic(
                            "minimizers_per_fragment build: unsupported sketch backend");
                    }
                    for (const minimap_sketch_data& minimizer : collector.extract_sketches_data()) {
                        const sequence_size_t global_pos =
                            static_cast<sequence_size_t>(minimizer.last_pos()) + emit_base;
                        if (global_pos < core_lo || global_pos >= core_hi) {
                            continue;
                        }
                        core.push_back(
                            minimap_sketch_data{minimizer.kmer(),
                                                static_cast<std::uint64_t>(global_pos - core_lo),
                                                minimizer.strand()});
                    }
                }
            } else if (execution_stats_out != nullptr) {
                ++execution_stats_out->short_core_runs;
            }
            minimizer_hashmap_fragment_index frag_idx;
            frag_idx.build(core, core_hi - core_lo); // Length of this fragment core.
            out_blob.resize(frag_idx.serialize_binary_size());
            frag_idx.serialize_to_binary(out_blob.data());
        };

        // Replace one fragment chunk with its complete serialized blob.
        auto write_fragment_blob = [&storage](std::size_t fragment_no,
                                              const std::vector<char>& blob) {
            const std::size_t bytes = blob.size();
            const index_sub_key_t fragment_key = static_cast<index_sub_key_t>(fragment_no);
            storage.resize(fragment_key, bytes);
            storage.template write_to_each<char>(
                fragment_key,
                [&blob, bytes](std::size_t chunk_no, char* buf, std::size_t cap_units) {
                    DEBUG_ASSERT(chunk_no == 0 && cap_units >= bytes,
                                 "one fragment index fits one chunk");
                    if (bytes > 0) {
                        std::memcpy(buf, blob.data(), bytes);
                    }
                    return chunk_no;
                });
        };

        // Build and write one fragment on the calling thread.
        auto build_and_write_serial = [&build_fragment_blob,
                                       &write_fragment_blob](std::size_t fragment_no) {
            sketch_collector collector;
            std::vector<minimap_sketch_data> core;
            std::vector<char> blob;
            build_fragment_blob(fragment_no, collector, core, blob);
            write_fragment_blob(fragment_no, blob);
        };

        auto& pool = bioseq::parallelism::global_pool::instance();
        const bool may_parallel_fragments =
            execution_stats_out == nullptr &&
            bioseq::parallelism::has_parallel_tasks(pool.get_mode()) &&
            pool.maximum_concurrency() > 1 && bioseq::storage::is_worker_shareable(seq) &&
            nfrag >= bioseq::parallelism::task_parallel_minimums::k_fragment_count;

        if (!may_parallel_fragments) {
            for (std::size_t fragment_no = 0; fragment_no < nfrag; ++fragment_no) {
                build_and_write_serial(fragment_no);
            }
        } else {
            const std::uint64_t build_peak =
                bioseq::parallelism::minimizer_index_peak_bytes_for_bp(frag_bp);
            const std::uint64_t peak_per_fragment = bioseq::common::saturating_mul(2, build_peak);

            for (std::size_t wave_lo = 0; wave_lo < nfrag;) {
                auto wave = bioseq::parallelism::try_reserve_budgeted_wave(
                    pool.maximum_concurrency(), peak_per_fragment);
                if (!wave.is_admitted) {
                    build_and_write_serial(wave_lo);
                    ++wave_lo;
                    continue;
                }

                const std::size_t wave_hi =
                    std::min(nfrag, wave_lo + static_cast<std::size_t>(wave.fanout));
                std::vector<std::vector<char>> blobs(wave_hi - wave_lo);

                bioseq::parallelism::parallel_for_pipeline(
                    0,
                    blobs.size(),
                    1,
                    bioseq::parallelism::task_parallel_minimums::k_fragment_count,
                    bioseq::parallelism::task_parallel_region::fragment_build,
                    [&build_fragment_blob, &blobs, wave_lo](std::size_t lo, std::size_t hi) {
                        sketch_collector collector;
                        std::vector<minimap_sketch_data> core;
                        for (std::size_t rel = lo; rel < hi; ++rel) {
                            build_fragment_blob(wave_lo + rel, collector, core, blobs[rel]);
                        }
                    });

                for (std::size_t rel = 0; rel < blobs.size(); ++rel) {
                    write_fragment_blob(wave_lo + rel, blobs[rel]);
                }
                wave_lo = wave_hi;
            }
        }
        // Write  the fragment count after all fragment blobs are complete.
        {
            const std::int64_t manifest = static_cast<std::int64_t>(nfrag);
            storage.resize(k_directory_sub_key, sizeof(std::int64_t));
            storage.template write_to_each<std::int64_t>(
                k_directory_sub_key,
                [manifest](std::size_t chunk_no, std::int64_t* manifest_buf, std::size_t) {
                    manifest_buf[0] = manifest;
                    return chunk_no;
                });
        }
    }

    /// Build using an explicit alhabet code and discard execution stats.
    /// @tparam p_alphabet Alphabet of the borrowed sequence.
    template <sequence_alphabet p_alphabet>
    static void build(p_storage& storage,
                      bioseq::storage::lazy_sequence_view<p_alphabet> seq,
                      minimizer_sketch_backend backend,
                      const bioseq::minimizers::minimizer_kernels* kernels,
                      const std::uint8_t alphabet_code) {
        build(storage, seq, backend, kernels, nullptr, alphabet_code);
    }

    /// Build fragments with the scalar minimizer backend.
    /// @tparam p_alphabet Alphabet carried by the borrowed lazy sequence.
    template <sequence_alphabet p_alphabet>
    static void build(p_storage& storage, bioseq::storage::lazy_sequence_view<p_alphabet> seq) {
        build(storage, seq, minimizer_sketch_backend::scalar, nullptr);
    }

    /// Clear the stored index and rebuild every fragment with the chosen backend.
    /// Check required kernel pointers before clearing. A later failure can
    /// leave the index incomplete.
    /// @tparam p_alphabet Alphabet of the borrowed sequence.
    template <sequence_alphabet p_alphabet>
    static void
    rebuild(p_storage& storage,
            bioseq::storage::lazy_sequence_view<p_alphabet> seq,
            minimizer_sketch_backend backend,
            const bioseq::minimizers::minimizer_kernels* kernels,
            std::uint8_t alphabet_code = bioseq::minimizers::k_unbound_packed_alphabet_code) {
        if (backend == minimizer_sketch_backend::exact_simd &&
            (kernels == nullptr || kernels->two_stack_nthash == nullptr)) {
            throw exact_simd_index_runtime_error{exact_simd_index_status::invalid_state,
                                                 "minimizers_per_fragment rebuild"};
        }
        if (backend == minimizer_sketch_backend::bounded_nthash_simd &&
            (kernels == nullptr || kernels->bounded_nthash == nullptr)) {
            throw packed_bounded_nthash_index_runtime_error{
                packed_bounded_nthash_index_status::invalid_state,
                "minimizers_per_fragment rebuild"};
        }
        storage.clear_entity();
        build(storage, seq, backend, kernels, nullptr, alphabet_code);
    }

    /// Clear the entity and rebuild all fragment blobs with the scalar backend.
    /// @tparam p_alphabet Alphabet carried by the borrowed lazy sequence.
    template <sequence_alphabet p_alphabet>
    static void rebuild(p_storage& storage, bioseq::storage::lazy_sequence_view<p_alphabet> seq) {
        rebuild(storage, seq, minimizer_sketch_backend::scalar, nullptr);
    }

    /// Assemble an index from fragments [frag_lo, frag_hi).
    /// Convert their positions to global coordinates and keep those within
    /// [clip_lo_bp, clip_hi_bp). Check the fragment-count  manifest and require
    /// every requested fragment. Reuse scratch for the collected pairs, then
    /// build the returned index from them.
    ///
    /// @param frag_lo First fragment to load.
    /// @param frag_hi One past the last fragment to load
    /// @param clip_lo_bp Inclusive ulobal position bound.
    /// @param clip_hi_bp Exclusive global position bound.
    static minimizer_hashmap_fragment_index assemble_segment(p_storage& storage,
                                                             std::size_t frag_lo,
                                                             std::size_t frag_hi,
                                                             sequence_size_t frag_bp,
                                                             sequence_size_t total_bp,
                                                             sequence_size_t clip_lo_bp,
                                                             sequence_size_t clip_hi_bp,
                                                             minimizer_assembly_scratch& scratch) {
        auto& pairs = scratch.pairs;
        scratch.clear_segment();
        const std::size_t expected_nfrag =
            static_cast<std::size_t>((total_bp + frag_bp - 1) / frag_bp);
        if (storage.number_of_chunks(k_directory_sub_key) != 1)
            storage.index_error("minimizers_per_fragment: missing manifest");
        const auto* manifest_p = storage.template read_chunk<std::int64_t>(k_directory_sub_key, 0);
        if (*manifest_p != static_cast<std::int64_t>(expected_nfrag))
            storage.index_error("minimizers_per_fragment: manifest nfrag mismatch");
        if (frag_hi > expected_nfrag)
            storage.index_error("minimizers_per_fragment: requested fragment past manifest nfrag");
        index_sub_key_t expected_f = static_cast<index_sub_key_t>(frag_lo);
        storage.read_chunks_in_range(
            static_cast<index_sub_key_t>(frag_lo),
            static_cast<index_sub_key_t>(frag_hi),
            [&storage, &expected_f, &frag_bp, &clip_lo_bp, &clip_hi_bp, &pairs](
                index_sub_key_t f, const char* blob, std::size_t blob_bytes) {
                if (f != expected_f)
                    storage.index_error(
                        "minimizers_per_fragment: missing or duplicate fragment chunk");
                ++expected_f;
                const sequence_size_t off = static_cast<sequence_size_t>(f) * frag_bp;
                storage.validate_minimizer_blob_or_error(blob, blob_bytes);
                wire_for_each_entry(
                    blob,
                    blob_bytes,
                    [&off, &clip_lo_bp, &clip_hi_bp, &pairs](std::uint64_t kmer,
                                                             std::uint64_t local_encoded) {
                        const sequence_size_t gpos =
                            static_cast<sequence_size_t>(local_encoded >> 1u) + off;
                        if (gpos < clip_lo_bp || gpos >= clip_hi_bp)
                            return; // Keep only the requested collection range.
                        pairs.emplace_back(kmer,
                                           (static_cast<std::uint64_t>(gpos) << 1u) |
                                               (local_encoded & 1ULL));
                    });
            });
        if (expected_f != static_cast<index_sub_key_t>(frag_hi))
            storage.index_error("minimizers_per_fragment: missing trailing fragment chunk");
        minimizer_hashmap_fragment_index merged;
        merged.build_from_pairs(pairs, total_bp); // Anchors use coordinates in the full target.
        return merged;
    }

    /// Assemble matching positions after the caller clears pairs and supplies
    /// sorted unique query hashes in scratch.
    static minimizer_hashmap_fragment_index assemble_segment_for_sorted_query_(
        p_storage& storage,
        std::size_t frag_lo,
        std::size_t frag_hi,
        sequence_size_t frag_bp,
        sequence_size_t total_bp,
        sequence_size_t clip_lo_bp,
        sequence_size_t clip_hi_bp,
        minimizer_assembly_scratch& scratch) {
        auto& pairs = scratch.pairs;
        auto& qkmers = scratch.query_kmers;
        clip_hi_bp = std::min(clip_hi_bp, total_bp);
        if (clip_lo_bp > clip_hi_bp) {
            clip_lo_bp = clip_hi_bp;
        }

        const std::size_t expected_nfrag =
            static_cast<std::size_t>((total_bp + frag_bp - 1) / frag_bp);
        if (storage.number_of_chunks(k_directory_sub_key) != 1)
            storage.index_error("minimizers_per_fragment: missing manifest");
        const auto* manifest_p = storage.template read_chunk<std::int64_t>(k_directory_sub_key, 0);
        if (*manifest_p != static_cast<std::int64_t>(expected_nfrag))
            storage.index_error("minimizers_per_fragment: manifest nfrag mismatch");
        if (frag_hi > expected_nfrag)
            storage.index_error("minimizers_per_fragment: requested fragment past manifest nfrag");
        index_sub_key_t expected_f = static_cast<index_sub_key_t>(frag_lo);
        storage.read_chunks_in_range(
            static_cast<index_sub_key_t>(frag_lo),
            static_cast<index_sub_key_t>(frag_hi),
            [&storage, &expected_f, &frag_bp, &clip_lo_bp, &clip_hi_bp, &qkmers, &pairs](
                index_sub_key_t f, const char* blob, std::size_t blob_bytes) {
                if (f != expected_f)
                    storage.index_error(
                        "minimizers_per_fragment: missing or duplicate fragment chunk");
                ++expected_f;
                const sequence_size_t off = static_cast<sequence_size_t>(f) * frag_bp;
                storage.validate_minimizer_blob_or_error(blob, blob_bytes);
                wire_for_each_entry(
                    blob,
                    blob_bytes,
                    [&qkmers, &off, &clip_lo_bp, &clip_hi_bp, &pairs](std::uint64_t kmer,
                                                                      std::uint64_t local_encoded) {
                        if (!std::binary_search(qkmers.begin(), qkmers.end(), kmer))
                            return;
                        const sequence_size_t gpos =
                            static_cast<sequence_size_t>(local_encoded >> 1u) + off;
                        if (gpos < clip_lo_bp || gpos >= clip_hi_bp)
                            return;
                        pairs.emplace_back(kmer,
                                           (static_cast<std::uint64_t>(gpos) << 1u) |
                                               (local_encoded & 1ULL));
                    });
            });
        if (expected_f != static_cast<index_sub_key_t>(frag_hi))
            storage.index_error("minimizers_per_fragment: missing trailing fragment chunk");
        minimizer_hashmap_fragment_index merged;
        merged.build_from_pairs(pairs, total_bp);
        return merged;
    }

    /// Assemble  a segment index containing hashes present in the query.
    /// Load the requested fragments, convert positions to global coordinates,
    /// and clip them to the collection range. Keeping only matching hashes
    /// reduces the index prepared for GPU seed lookup.
    ///
    /// @param query_minimizers Query minimizers whose hashes select stored matches.
    /// @param frag_lo First fragment to load.
    /// @param frag_hi One past the last, fragment to load.
    /// @param clip_lo_bp Inclusive global position bound.
    /// @param clip_hi_bp Exclusive global position bound.
    static minimizer_hashmap_fragment_index
    assemble_segment_for_query(p_storage& storage,
                               std::span<const minimap_sketch_data> query_minimizers,
                               std::size_t frag_lo,
                               std::size_t frag_hi,
                               sequence_size_t frag_bp,
                               sequence_size_t total_bp,
                               sequence_size_t clip_lo_bp,
                               sequence_size_t clip_hi_bp,
                               minimizer_assembly_scratch& scratch) {
        scratch.clear_query();
        auto& qkmers = scratch.query_kmers;
        qkmers.reserve(query_minimizers.size());
        for (const minimap_sketch_data& minimizer : query_minimizers) {
            qkmers.push_back(minimizer.kmer());
        }
        std::sort(qkmers.begin(), qkmers.end());
        qkmers.erase(std::unique(qkmers.begin(), qkmers.end()), qkmers.end());
        return assemble_segment_for_sorted_query_(storage,
                                                  frag_lo,
                                                  frag_hi,
                                                  frag_bp,
                                                  total_bp,
                                                  clip_lo_bp,
                                                  clip_hi_bp,
                                                  scratch);
    }

    /// Assemble the query's segment index from any registered minimizer record  type.
    /// Decode query hashes once with for_each_decoded, then use the same filtered
    /// assembly path as the packed-record overload.
    static minimizer_hashmap_fragment_index
    assemble_segment_for_query(
        p_storage& storage,
        const bioseq::minimizers::minimizer_physical_batch_view query_minimizers,
        std::size_t frag_lo,
        std::size_t frag_hi,
        sequence_size_t frag_bp,
        sequence_size_t total_bp,
        sequence_size_t clip_lo_bp,
        sequence_size_t clip_hi_bp,
        minimizer_assembly_scratch& scratch) {
        if (bioseq::minimizers::find_minimizer_physical_record(
                query_minimizers.record_id()) == nullptr ||
            (!query_minimizers.empty() && query_minimizers.data_owner_address() == nullptr)) {
            storage.index_error("minimizers_per_fragment: invalid physical query owner");
        }
        scratch.clear_query();
        auto& qkmers = scratch.query_kmers;
        qkmers.reserve(query_minimizers.size());
        query_minimizers.for_each_decoded(
            [&qkmers](std::size_t,
                      const bioseq::minimizers::minimizer_physical_record_value minimizer) {
                qkmers.push_back(minimizer.kmer());
            });
        std::sort(qkmers.begin(), qkmers.end());
        qkmers.erase(std::unique(qkmers.begin(), qkmers.end()), qkmers.end());
        return assemble_segment_for_sorted_query_(storage,
                                                  frag_lo,
                                                  frag_hi,
                                                  frag_bp,
                                                  total_bp,
                                                  clip_lo_bp,
                                                  clip_hi_bp,
                                                  scratch);
    }
};

} // namespace bioseq::indexing

export namespace bioseq::indexing {
/// Registry descriptor for the per-fragment minimizer index.
template <> struct index_descriptor<minimizers_per_fragment_index> {
    /// Registry layout used by all concrete storage specializations.
    static constexpr sequence_index_layout index_type = sequence_index_layout::fragment_chunks;
    /// Stable registry name for per-fragment minimizer blobs.
    static constexpr std::string_view index_name = minimizer_hashmap_fragment_index::index_name;
    /// Permit building an independent index blob when a sequence fragment is inserted.
    static constexpr bool allows_update_on_insert =
        true;
};
static_assert(
    requires { index_descriptor<minimizers_per_fragment_index>::index_name; },
    "minimizers_per_fragment_index index_descriptor must be exported");
} // namespace bioseq::indexing
