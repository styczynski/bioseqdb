/**
 * @file minimizer_hashmap/fragment_producer.cppm
 * @brief Fragment replay, wire assembly, and per-fragment index production.
 */
module;

#include <libassert/assert.hpp>

export module bioseq.indexing.minimizer_hashmap:fragment_producer;

import std;
import :exact_simd;
import :grouped_soa;
import :prepared_reader;
import :compact_direct;
import :generation;
import bioseq.common.logging;
import bioseq.common.seqan_adapter;
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

/// Caller-owned per-row/per-query assembly scratch. Reused across
/// segments within one query, destroyed with its IndexReadContext.
struct minimizer_assembly_scratch {
    /// Globalized `(hash, encoded_position)` pairs for one assembly result.
    std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs;
    /// Deduplicated query kmer hashes used to filter persisted buckets.
    std::vector<std::uint64_t> query_kmers;
    /// Reusable position buffer passed to `wire_for_each_bucket`.
    std::vector<std::uint64_t> bucket_positions;
    /// Storage sub keys selected for query-touched bucket loads.
    std::vector<bioseq::indexing::index_sub_key_t> wanted_sub_keys;
    /// Clear state retained only for the current target segment.
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

// ---- shared serialized-index walkers -------------------------------------

/// Walk one persisted wire blob and emit each key/position pair.
/// Aborts via `index_corruption_panic` when the header, layout, or bucket ranges are invalid.
/// @tparam p_function Callable accepting `(hash, encoded_position)`.
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

/// Walk one persisted wire blob and emit each complete hash bucket.
/// Aborts via `index_corruption_panic` when the header, layout, or bucket ranges are invalid.
/// The callback receives a pointer into `pos_scratch`, which is resized per bucket and remains
/// valid only until the next bucket.
/// @tparam p_function Callable accepting `(hash, position_count, positions)`.
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

/// Return the number of whole fragments fitting one GPU window.
/// The bound is the minimum of fragment count, run-buffer memory, encoded position field,
/// and dispatch geometry limits.
///
/// @param nfrag Total physical fragment count for the target.
/// @param frag_bp Bytes in one fragment core.
/// @param halo_bp Collection halo on each side of a batch core.
/// @param run_buffer_bytes GPU run-buffer capacity for one submission.
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

/// Serialize sorted `(hash, encoded_position)` pairs into one fragment blob.
/// This function performs no minimizer collection or sorting.
///
/// @param target_len Fragment core length stored in the blob header.
/// @param out_blob Resized to hold the complete wire encoding.
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

/// Operation-originated route stats for one complete minimizer-fragment generation encode.
/// The grouped-SoA encoder replays its producer once each for planning,
/// control emission, and position emission. Every biological core records the
/// branch it actually entered.
/// Selected SIMD/GPU routes additionally carry their native checked stats, so a
/// caller cannot accept a backend merely because its enum was requested.
class minimizer_fragment_generation_execution_receipt {
  public:
    minimizer_sketch_backend selected_backend = minimizer_sketch_backend::scalar;
    minimizer_scheme_identity minimizer_scheme{};
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
    packed_bounded_nthash_index_status packed_bounded_status =
        packed_bounded_nthash_index_status::invalid_state;
    packed_bounded_nthash_index_execution_stats packed_bounded_stats{};
    bool packed_bounded_stats_present = false;
    bioseq::gpu::vulkan::vulkan_segment_pair_execution_stats vulkan_stats{};
    /// Set when route identity fields were populated from a bound producer.
    bool backend_bound = false;
    /// Set when a route counter overflowed and the receipt must be rejected.
    bool saturated = false;

    /// Return the successful replay count required by the selected layout.
    [[nodiscard]] std::uint64_t required_successful_scan_count() const noexcept {
        if (generation_layout == minimizer_generation_layout::grouped_soa_segment_local) {
            return 3u;
        }
        /* An unbound producer can still record two explicit direct replays.
         * Publication additionally requires a concrete matching storage layout. */
        return generation_layout == minimizer_generation_layout::invalid ||
                       is_supported_minimizer_generation_layout(generation_layout)
                   ? 2u
                   : 0u;
    }

    /// Check route identity, replay count, and selected backend counters.
    [[nodiscard]] bool is_accepted() const noexcept {
        // A fixed-width geometry retry contributes exactly one additional
        // planning scan. The successful-pass count is bound from the storage
        // layout before the first encoder scan.
        const std::uint64_t required_scans = required_successful_scan_count();
        if (!common_identity_accepted_() || required_scans == 0u ||
            geometry_retry_count > (std::numeric_limits<std::uint64_t>::max)() - required_scans ||
            producer_scan_count != geometry_retry_count + required_scans) {
            return false;
        }
        return selected_route_accepted_(requires_backend_work_());
    }

    /// Accept stats emitted by the direct minimizer-fragment builder, which is one
    /// biological construction operation rather than an encoder replay and
    /// therefore deliberately has no storage-layout/publication binding.
    [[nodiscard]] bool direct_build_accepted() const noexcept {
        return common_identity_accepted_() && producer_scan_count == 1u &&
               geometry_retry_count == 0u &&
               generation_layout == minimizer_generation_layout::invalid && window_size == 10u &&
               kmer_size == 15u && !route_snapshot_present_ && !publication_binding_present_ &&
               selected_route_accepted_(requires_backend_work_());
    }

    /// Check whether this receipt authorizes one exact encoded generation.
    [[nodiscard]] bool
    publication_accepted(const minimizer_generation_publication_binding& expected) const noexcept {
        return is_accepted() && route_snapshot_matches_() && publication_binding_present_ &&
               generation_layout != minimizer_generation_layout::invalid &&
               generation_layout == expected.layout && expected.valid() &&
               publication_binding_ == expected;
    }

  private:
    /// Check stable backend, scheme, target, and parameter identity fields.
    [[nodiscard]] bool common_identity_accepted_() const noexcept {
        return backend_bound && !saturated && target_length != 0u && window_size != 0u &&
               kmer_size != 0u &&
               minimizer_scheme == minimizer_scheme_identity_for_backend(selected_backend);
    }

    /// Test whether the target is long enough to require a minimizer backend invocation.
    [[nodiscard]] bool requires_backend_work_() const noexcept {
        const std::uint64_t minimum_work_bases =
            static_cast<std::uint64_t>(window_size) + kmer_size - 1u;
        return target_length >= minimum_work_bases;
    }

    /// Validate that only the selected backend ran and that its native receipt is consistent.
    [[nodiscard]] bool selected_route_accepted_(bool require_backend_work) const noexcept {
        const bool no_exact = exact_simd_core_runs == 0 && !exact_simd_stats_present;
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
            return scalar_core_runs == 0 && bounded_nthash_core_runs == 0 &&
                   vulkan_core_runs == 0 && no_bounded && no_vulkan &&
                   (exact_simd_core_runs == 0
                        ? (!require_backend_work && !exact_simd_stats_present)
                        : (exact_simd_stats_present &&
                           exact_simd_stats.is_internally_consistent(require_backend_work)));
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

    /// Immutable route fields captured before the publication binding is attached.
    struct route_snapshot {
        minimizer_sketch_backend selected_backend = minimizer_sketch_backend::scalar;
        minimizer_scheme_identity minimizer_scheme{};
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

    /// Compare the Vulkan counters that participate in publication authority.
    [[nodiscard]] static bool same_vulkan_stats_(
        const bioseq::gpu::vulkan::vulkan_segment_pair_execution_stats& left,
        const bioseq::gpu::vulkan::vulkan_segment_pair_execution_stats& right) noexcept {
        return left.completed_roundtrips == right.completed_roundtrips &&
               left.downloaded_minimizers == right.downloaded_minimizers &&
               left.gpu_window_splits == right.gpu_window_splits &&
               left.saturated == right.saturated;
    }

    /// Freeze the complete route receipt before generation metadata is bound.
    void capture_route_snapshot_() noexcept {
        route_snapshot_ = route_snapshot{
            .selected_backend = selected_backend,
            .minimizer_scheme = minimizer_scheme,
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
               minimizer_scheme == value.minimizer_scheme && target_length == value.target_length &&
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
               packed_bounded_status == value.packed_bounded_status &&
               packed_bounded_stats == value.packed_bounded_stats &&
               packed_bounded_stats_present == value.packed_bounded_stats_present &&
               same_vulkan_stats_(vulkan_stats, value.vulkan_stats) &&
               backend_bound == value.backend_bound && saturated == value.saturated;
    }

    /// Bind an unchanged route snapshot to one encoded grouped-SoA generation.
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
            backend.revision == generation.manifest.producer_identity_revision;
    }
};

/// Concrete minimizer-fragment producer. Each replay rescans source fragments with
/// the canonical `w+k-2` halo and emits each physical fragment's independently
/// ordered index. A multi-run merge route is used only when a caller explicitly
/// requests a persisted span wider than one
/// physical fragment. The production minimizer-fragment publisher never does so.
///
/// @tparam p_alphabet Alphabet carried by the borrowed lazy sequence.
template <sequence_alphabet p_alphabet> class minimizer_fragment_generation_replay_producer {
  private:
    using sequence_type = bioseq::storage::lazy_sequence_view<p_alphabet>;
    using traits = typename sequence_type::traits;

    sequence_type sequence_;
    minimizer_sketch_backend backend_ = minimizer_sketch_backend::scalar;
    const bioseq::minimizers::minimizer_kernels* kernels_ = nullptr;
    std::uint8_t alphabet_code_ = bioseq::minimizers::k_unbound_packed_alphabet_code;
    std::uint16_t window_size_ = 10;
    std::uint16_t kmer_size_ = 15;
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

    /// Increment a route counter and latch saturation instead of wrapping.
    void increment_route_counter_(std::uint64_t& counter) noexcept {
        if (counter == (std::numeric_limits<std::uint64_t>::max)()) {
            route_stats_saturated_ = true;
            return;
        }
        ++counter;
    }

    /// Merge one bounded ntHash result into the producer-wide route receipt.
    void
    record_packed_bounded_result(const packed_bounded_nthash_index_bridge_result& result) noexcept {
        packed_bounded_status_ = result.status;
        packed_bounded_stats_.merge(result.stats);
        packed_bounded_stats_present_ = true;
    }

    /// Collect minimizers for one half-open core `[core_lo, core_hi)` with the standard halo.
    /// Events are sorted and deduplicated before return.
    /// Returns `storage_contract_mismatch` when the selected backend or SIMD collection fails.
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
                    &materialized_window_bp);
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
                    route_stats_saturated_ = route_stats_saturated_ || exact_simd_stats_.saturated;
                } catch (const exact_simd_index_runtime_error&) {
                    return std::unexpected(
                        minimizer_generation_encoding_error::storage_contract_mismatch);
                }
            } else if (backend_ == minimizer_sketch_backend::scalar) {
                increment_route_counter_(scalar_core_runs_);
                bioseq::simd::collect_minimizers_segment_scalar<minimap_sketch_data>(
                    collector, sequence_, collect_lo, collect_hi, window_size_, kmer_size_);
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
        std::ranges::sort(run, {}, [](const canonical_minimizer_event& event) {
            return std::pair{event.hash, event.encoded_position};
        });
        // Minimizer collectors may report one selected occurrence from more
        // than one adjacent tied window. Persisted occurrence semantics are
        // canonical `(hash, position, strand)` events for every backend, so
        // collapse identical reports inside this backend-owned core run after
        // the mandatory sort. Core ownership is disjoint, therefore a
        // duplicate cannot legitimately need to survive in another run. The
        // lower wire encoder remains fail-closed for arbitrary producers that
        // violate this canonical-stream contract.
        run.erase(std::ranges::unique(run).begin(), run.end());
        return run;
    }

  public:
    /// Bind a borrowed sequence, selected backend, kernel table, and minimizer tuple.
    /// The sequence storage and kernel table must outlive all producer replays.
    minimizer_fragment_generation_replay_producer(
        sequence_type sequence,
        minimizer_sketch_backend backend,
        const bioseq::minimizers::minimizer_kernels* kernels,
        std::uint8_t alphabet_code,
        std::uint16_t window_size = 10,
        std::uint16_t kmer_size = 15) noexcept
        : sequence_(sequence), backend_(backend), kernels_(kernels), alphabet_code_(alphabet_code),
          window_size_(window_size), kmer_size_(kmer_size) {}

    /// Bind the required replay count from the persisted generation layout before scanning.
    /// Sets `saturated` when the layout is unsupported, a scan already ran, or the layout changes
    /// after binding.
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

    /// Replay canonical events in hash-major order for one persisted segment geometry.
    ///
    /// Each call rescans the borrowed source. A changed `persisted_span` is recorded as a
    /// geometry retry. The receiver is invoked once per event in sorted order. Returning an
    /// error stops the scan and propagates that error.
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
        if ((backend_ == minimizer_sketch_backend::exact_simd &&
             (kernels_ == nullptr || kernels_->two_stack_nthash == nullptr)) ||
            (backend_ == minimizer_sketch_backend::bounded_nthash_simd &&
             (kernels_ == nullptr || kernels_->bounded_nthash == nullptr))) {
            return std::unexpected(minimizer_generation_encoding_error::storage_contract_mismatch);
        }
        const std::uint64_t target_length = sequence_.bp_size();

        std::uint64_t segment_lo = 0;
        while (segment_lo < target_length) {
            const std::uint64_t segment_width =
                std::min(persisted_span, target_length - segment_lo);
            const std::uint64_t segment_hi = segment_lo + segment_width;
            std::vector<std::vector<canonical_minimizer_event>> runs;
            std::uint64_t buffered_events = 0;
            std::uint64_t fragment_lo = (segment_lo / fragment_bp) * fragment_bp;
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
                    if (buffered_events >
                        (std::numeric_limits<std::uint64_t>::max)() - run->size()) {
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
            peak_buffered_events_ = std::max(peak_buffered_events_, buffered_events);
            peak_buffered_core_bp_ = std::max(peak_buffered_core_bp_, segment_width);
            peak_active_fragment_runs_ =
                std::max<std::uint64_t>(peak_active_fragment_runs_, runs.size());

            if (runs.size() == 1u) {
                for (const canonical_minimizer_event& event : runs.front()) {
                    if (auto accepted = receiver(event); !accepted) {
                        return accepted;
                    }
                }
                segment_lo = segment_hi;
                continue;
            }

            /// Current event ordinal within one independently sorted fragment run.
            struct cursor {
                std::size_t run = 0;
                std::size_t index = 0;
            };
            // Order the merge heap by hash and encoded position.
            const auto after = [&runs](const cursor& lhs, const cursor& rhs) {
                const auto& left = runs[lhs.run][lhs.index];
                const auto& right = runs[rhs.run][rhs.index];
                return std::pair{left.hash, left.encoded_position} >
                       std::pair{right.hash, right.encoded_position};
            };
            std::priority_queue<cursor, std::vector<cursor>, decltype(after)> heap{after};
            for (std::size_t run = 0; run < runs.size(); ++run) {
                heap.push(cursor{.run = run, .index = 0});
            }
            while (!heap.empty()) {
                cursor current_cursor = heap.top();
                heap.pop();
                if (auto accepted = receiver(runs[current_cursor.run][current_cursor.index]);
                    !accepted) {
                    return accepted;
                }
                ++current_cursor.index;
                if (current_cursor.index < runs[current_cursor.run].size()) {
                    heap.push(current_cursor);
                }
            }
            segment_lo = segment_hi;
        }
        return {};
    }

    /// Return the number of attempted producer scans, including geometry retries.
    [[nodiscard]] std::uint64_t scan_count() const noexcept {
        return scan_count_;
    }
    /// Return the maximum canonical events buffered for one persisted segment.
    [[nodiscard]] std::uint64_t peak_buffered_events() const noexcept {
        return peak_buffered_events_;
    }
    /// Return the largest persisted segment width buffered in base pairs.
    [[nodiscard]] std::uint64_t peak_buffered_core_bp() const noexcept {
        return peak_buffered_core_bp_;
    }
    /// Return the largest number of fragment runs merged concurrently.
    [[nodiscard]] std::uint64_t peak_active_fragment_runs() const noexcept {
        return peak_active_fragment_runs_;
    }
    /// Return the largest bounded ntHash window materialized in base pairs.
    [[nodiscard]] std::uint64_t peak_bounded_materialized_window_bp() const noexcept {
        return peak_bounded_materialized_window_bp_;
    }
    /// Test whether an exact SIMD core produced a native execution receipt.
    [[nodiscard]] bool exact_simd_stats_present() const noexcept {
        return exact_simd_stats_present_;
    }
    /// Borrow the merged exact SIMD execution receipt.
    [[nodiscard]] const exact_simd_index_execution_stats& exact_simd_stats() const noexcept {
        return exact_simd_stats_;
    }
    /// Test whether a bounded ntHash core produced a native execution receipt.
    [[nodiscard]] bool packed_bounded_stats_present() const noexcept {
        return packed_bounded_stats_present_;
    }
    /// Return the most recently merged bounded ntHash status.
    [[nodiscard]] packed_bounded_nthash_index_status packed_bounded_status() const noexcept {
        return packed_bounded_status_;
    }
    /// Borrow the merged bounded ntHash execution receipt.
    [[nodiscard]] const packed_bounded_nthash_index_execution_stats&
    packed_bounded_stats() const noexcept {
        return packed_bounded_stats_;
    }
    /// Snapshot route counters and identities for publication validation.
    [[nodiscard]] minimizer_fragment_generation_execution_receipt execution_stats() const noexcept {
        minimizer_fragment_generation_execution_receipt stats;
        stats.selected_backend = backend_;
        stats.minimizer_scheme = scheme_identity();
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
        stats.packed_bounded_status = packed_bounded_status_;
        stats.packed_bounded_stats = packed_bounded_stats_;
        stats.packed_bounded_stats_present = packed_bounded_stats_present_;
        stats.vulkan_stats = vulkan_stats_;
        stats.backend_bound = true;
        stats.saturated = route_stats_saturated_ || vulkan_stats_.saturated;
        stats.capture_route_snapshot_();
        return stats;
    }
};

/// Storage-bound per-fragment minimizer index operations.
/// @tparam p_storage Chunked storage implementing the fragment-chunk index contract.
template <class p_storage> struct minimizers_per_fragment_index {
    static_assert(is_index_chunked_storage_for<p_storage, sequence_index_layout::fragment_chunks>);
    /// Registry layout used for one serialized blob per physical fragment.
    static constexpr sequence_index_layout index_type = sequence_index_layout::fragment_chunks;
    /// Stable registry name inherited from the fragment hash-map representation.
    static constexpr std::string_view index_name = minimizer_hashmap_fragment_index::index_name;
    /// Concrete chunked storage type bound to these operations.
    using storage_type = p_storage;

    /// Construct the replay producer used by grouped-SoA publication.
    /// @tparam p_alphabet Alphabet carried by the borrowed lazy sequence.
    template <sequence_alphabet p_alphabet>
    [[nodiscard]] static auto make_generation_replay_producer(
        bioseq::storage::lazy_sequence_view<p_alphabet> sequence,
        minimizer_sketch_backend backend,
        const bioseq::minimizers::minimizer_kernels* kernels,
        std::uint8_t alphabet_code = bioseq::minimizers::k_unbound_packed_alphabet_code) {
        return minimizer_fragment_generation_replay_producer<p_alphabet>{
            sequence, backend, kernels, alphabet_code};
    }

    /// Encode a fragment image from the bounded producer selected by the caller.
    ///
    /// The sink receives the grouped-SoA generation rows. Route statistics are bound to the
    /// resulting manifest and fail closed if backend identity or replay counts disagree.
    ///
    /// @tparam p_alphabet Alphabet carried by the borrowed lazy sequence.
    /// @tparam p_sink Row sink that accepts the encoded generation stream.
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
        std::uint8_t runtime_alphabet_code = bioseq::minimizers::k_unbound_packed_alphabet_code) {
        if (execution_stats_out != nullptr) {
            *execution_stats_out = {};
        }
        if (options.window_size != 10 || options.kmer_size != 15) {
            return std::unexpected(minimizer_generation_encoding_error::storage_contract_mismatch);
        }
        if ((backend == minimizer_sketch_backend::exact_simd &&
             (kernels == nullptr || kernels->two_stack_nthash == nullptr)) ||
            (backend == minimizer_sketch_backend::bounded_nthash_simd &&
             (kernels == nullptr || kernels->bounded_nthash == nullptr))) {
            return std::unexpected(minimizer_generation_encoding_error::storage_contract_mismatch);
        }
        auto producer =
            make_generation_replay_producer(sequence, backend, kernels, runtime_alphabet_code);
        auto encoded = encode_minimizer_generation(producer, sink, options, limits);
        minimizer_fragment_generation_execution_receipt stats = producer.execution_stats();
        const minimizer_producer_identity stats_backend =
            minimizer_producer_backend_identity_for_backend(stats.selected_backend);
        if (encoded && (encoded->manifest.producer_backend_id != stats_backend.id ||
                        encoded->manifest.producer_identity_revision != stats_backend.revision)) {
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

    /// Build and publish one native fragment blob per 32 KiBP target core.
    ///
    /// Positions in each blob are fragment-local and preserve the strand bit. The completeness
    /// manifest is written after every fragment. Storage may be partially modified if collection
    /// or a storage callback fails.
    ///
    /// @tparam p_alphabet Alphabet carried by the borrowed lazy sequence.
    template <sequence_alphabet p_alphabet>
    static void
    build(p_storage& storage,
          bioseq::storage::lazy_sequence_view<p_alphabet> seq,
          minimizer_sketch_backend backend,
          const bioseq::minimizers::minimizer_kernels* kernels,
          minimizer_fragment_generation_execution_receipt* execution_stats_out = nullptr,
          std::uint8_t alphabet_code = bioseq::minimizers::k_unbound_packed_alphabet_code) {
        constexpr std::size_t w = 10, k = 15; // Persisted minimizer tuple.
        constexpr std::size_t frag_bp = 32'768u;
        constexpr std::size_t k_context_span = w + k; // backward context
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

        // Batch canonical 32 KiBP fragments into large GPU collection windows,
        // then scatter the one sorted global pair stream back into persisted
        // fragment-local blobs.  One synchronous submission per fragment is a
        // severe regression and defeats the configured GPU minimizer-fragment batch geometry.
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

        // Collect one haloed core and serialize its fragment-local minimizer index.
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
            frag_idx.build(core, core_hi - core_lo); // fragment-local target_len
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

        // Run one fragment through reusable serial collection and immediate publication.
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
        // Publish the fragment-count completeness manifest after every fragment blob.
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

    /// Build fragments with an explicit packed-alphabet code and no execution receipt.
    /// @tparam p_alphabet Alphabet carried by the borrowed lazy sequence.
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

    /// Clear the entity and rebuild all fragment blobs with an explicit backend.
    ///
    /// Clearing happens before collection, so a later failure leaves the entity incomplete.
    /// @tparam p_alphabet Alphabet carried by the borrowed lazy sequence.
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

    /// Assemble all persisted fragment buckets intersecting one target segment.
    ///
    /// Fragment-local positions are globalized, clipped to `[clip_lo_bp, clip_hi_bp)`, and
    /// rebuilt into one target-wide hash map using caller-owned reusable scratch.
    ///
    /// @param frag_lo First fragment index in `[frag_lo, frag_hi)`.
    /// @param frag_hi One-past-last fragment index to load.
    /// @param clip_lo_bp Inclusive global clip start for retained positions.
    /// @param clip_hi_bp Exclusive global clip end for retained positions.
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
                            return; // CLIP to the segment's collection range
                        pairs.emplace_back(kmer,
                                           (static_cast<std::uint64_t>(gpos) << 1u) |
                                               (local_encoded & 1ULL));
                    });
            });
        if (expected_f != static_cast<index_sub_key_t>(frag_hi))
            storage.index_error("minimizers_per_fragment: missing trailing fragment chunk");
        minimizer_hashmap_fragment_index merged;
        merged.build_from_pairs(pairs, total_bp); // FULL target length
        return merged;
    }

    /// Query-filtered segment assembly for GPU seed lookup. Only query kmers
    /// can be probed by the seed stage, so persisted minimizer-fragment does not need to
    /// materialise unrelated buckets before uploading the segment index.
    ///
    /// @param query_minimizers Query sketches whose kmer hashes select persisted buckets.
    /// @param frag_lo First fragment index in `[frag_lo, frag_hi)`.
    /// @param frag_hi One-past-last fragment index to load.
    /// @param clip_lo_bp Inclusive global clip start for retained positions.
    /// @param clip_hi_bp Exclusive global clip end for retained positions.
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
        auto& pairs = scratch.pairs;
        auto& qkmers = scratch.query_kmers;
        qkmers.reserve(query_minimizers.size());
        for (const minimap_sketch_data& qm : query_minimizers) {
            qkmers.push_back(qm.kmer());
        }
        std::sort(qkmers.begin(), qkmers.end());
        qkmers.erase(std::unique(qkmers.begin(), qkmers.end()), qkmers.end());

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
};

} // namespace bioseq::indexing

export namespace bioseq::indexing {
/// Registry descriptor for the per-fragment minimizer index.
template <> struct index_descriptor<minimizers_per_fragment_index> {
    /// Registry layout used by all concrete storage specializations.
    static constexpr sequence_index_layout index_type = sequence_index_layout::fragment_chunks;
    /// Stable registry name for per-fragment minimizer blobs.
    static constexpr std::string_view index_name = minimizer_hashmap_fragment_index::index_name;
    /// Allow each newly inserted sequence fragment to build its own independent blob.
    static constexpr bool allows_update_on_insert =
        true;
};
static_assert(
    requires { index_descriptor<minimizers_per_fragment_index>::index_name; },
    "minimizers_per_fragment_index index_descriptor must be exported");
} // namespace bioseq::indexing
