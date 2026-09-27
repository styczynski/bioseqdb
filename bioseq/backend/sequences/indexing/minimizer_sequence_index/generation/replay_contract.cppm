/**
 * @file minimizer_sequence_index/generation/replay_contract.cppm
 * @brief Checks that spill replays use the same minimizer events and generation settings.
 */
export module bioseq.indexing.minimizer_sequence.generation:replay_contract;

import std;
import bioseq.pipeline.runtime_contract;
import bioseq.indexing.minimizer_sequence.admitted_runs;
import bioseq.indexing.minimizer_sequence.bucket_codec;
import bioseq.common.checked_arithmetic;
import bioseq.common.hashing;
import bioseq.gpu.vulkan.abi;
import bioseq.indexing.minimizer_hashmap;
import bioseq.indexing.minimizer_index;
import bioseq.indexing.storage;
import bioseq.sequences.kmers.highway_minimizer_contract;
import bioseq.sequences.kmers.vulkan_minimizers;
import bioseq.storage.size;

export namespace bioseq::indexing {

using bioseq::minimizers::minimizer_sketch_backend;
using bioseq::storage::sequence_size_t;

/** Callback used to check the storage merge interface at compile time. */
struct direct_minimizer_segment_merge_stats {
    /** Check access to the position  count and batches, discarding the data. */
    template <class p_positions>
    void operator()(std::uint32_t, std::uint64_t, p_positions&& positions) const {
        (void)positions.total_count();
        positions.for_each_batch([](std::span<const std::uint64_t>) noexcept {});
    }
};

/**
 * @brief Requires spill storage with generation IDs, sorted merging and segment rerouting.
 * @tparam p_storage Candidate spill storage type
 */
template <class p_storage>
concept direct_minimizer_qualified_spill_storage =
    requires(p_storage& storage,
             std::uint32_t position_segment,
             std::uint64_t span,
             std::uint64_t target_length,
             std::span<const detail::sorted_pair_t> run) {
        {
            storage.begin_segment_spill_generation()
        } -> std::same_as<direct_minimizer_spill_storage_binding>;
        {
            std::as_const(storage).segment_spill_storage_binding()
        } -> std::same_as<direct_minimizer_spill_storage_binding>;
        {
            storage.spill_sorted_segment_run(position_segment, run)
        } -> std::convertible_to<std::size_t>;
        {
            storage.merge_segment_runs(direct_minimizer_segment_merge_stats{})
        } -> std::same_as<void>;
        { storage.reroute_segment_runs(span, span, target_length) } -> std::convertible_to<bool>;
    };

/** Producer that replays spill runs checked by the generation builder. */
template <class p_storage> class direct_minimizer_external_spill_replay_producer;

/** Replay adapter used to check spill data before a generation receipt exists. */
template <class p_storage> class direct_minimizer_unbound_external_spill_replay_producer;

/** Operations that collect spill runs and replay them for index generation. */
template <class p_storage, class p_result_owner = void>
struct direct_minimizer_generation_replay;

/// SHA-256 digest of hash/position pairs in spill replay order.
using direct_minimizer_content_digest = bioseq::common::sha256_digest;

/**
 * @brief Records how a  minimizer, index was collected, replayed and encoded.
 * Collection writes spill runs once. Planning and output read those runs again
 * Separate counters record this work, while private snapshots detect changes
 * to the public fields after the spill or completed index has been checked.
 */
class minimizer_sequence_generation_execution_receipt {
  public:
    minimizer_sketch_backend selected_backend = minimizer_sketch_backend::scalar;
    minimizer_scheme_identity minimizer_scheme{};
    minimizer_generation_algorithm_identity algorithm_identity{};
    std::uint64_t target_length = 0;
    std::uint16_t window_size = 0;
    std::uint16_t kmer_size = 0;
    std::uint64_t configured_core_span_bp = 0;
    std::uint64_t peak_core_span_bp = 0;
    std::uint64_t peak_core_events = 0;
    std::uint64_t peak_bounded_materialized_window_bp = 0;
    std::uint64_t spill_run_count = 0;
    std::uint64_t spilled_event_count = 0;
    direct_minimizer_content_digest spill_content_digest{};
    std::uint64_t spill_operation_epoch = 0;
    direct_minimizer_spill_storage_binding spill_storage_binding{};
    std::uint64_t spill_qualification_scan_count = 0;
    std::uint64_t spill_qualification_merge_count = 0;
    std::uint64_t short_core_runs = 0;
    std::uint64_t scalar_core_runs = 0;
    std::uint64_t exact_simd_core_runs = 0;
    std::uint64_t bounded_nthash_core_runs = 0;
    std::uint64_t vulkan_core_runs = 0;
    exact_simd_index_execution_stats exact_simd_stats{};
    bool exact_simd_stats_available = false;
    reference_simd_index_execution_stats reference_simd_stats{};
    bool reference_simd_stats_available = false;
    packed_bounded_nthash_index_status packed_bounded_status =
        packed_bounded_nthash_index_status::invalid_state;
    packed_bounded_nthash_index_execution_stats packed_bounded_stats{};
    bool packed_bounded_stats_available = false;
    bioseq::gpu::vulkan::vulkan_segment_pair_execution_stats vulkan_stats{};
    minimizer_generation_layout generation_layout = minimizer_generation_layout::invalid;
    std::uint64_t replay_scan_count = 0;
    std::uint64_t replay_merge_pass_count = 0;
    std::uint64_t replay_reroute_pass_count = 0;
    std::uint64_t replay_completed_scan_count = 0;
    std::uint64_t replay_failed_scan_count = 0;
    std::uint64_t replay_binding_checked_failed_scan_count = 0;
    std::uint64_t replay_illegal_span_transition_count = 0;
    std::uint64_t replay_first_completed_event_count = 0;
    std::uint64_t replay_last_completed_event_count = 0;
    direct_minimizer_content_digest replay_first_completed_content_digest{};
    direct_minimizer_content_digest replay_last_completed_content_digest{};
    std::uint64_t replay_final_span_bp = 0;
    direct_minimizer_spill_storage_binding replay_final_storage_binding{};
    std::uint64_t replay_peak_callback_batch_events = 0;
    std::uint64_t replay_visited_event_count = 0;
    std::uint64_t manifest_position_count = 0;
    std::uint64_t manifest_position_segment_span_bp = 0;
    bool spill_complete = false;
    bool backend_bound = false;
    bool manifest_bound = false;
    bool saturated = false;

    /** Return the required replay count: three for grouped-SoA, two before a layout is bound. */
    [[nodiscard]] std::uint64_t required_successful_replay_count() const noexcept {
        if (generation_layout == minimizer_generation_layout::grouped_soa_segment_local) {
            return 3u;
        }
        return generation_layout == minimizer_generation_layout::invalid ||
                       is_supported_minimizer_generation_layout(generation_layout)
                   ? 2u
                   : 0u;
    }

    /** Add to a counter, uetting it to its maximum and flagging overflow if needed. */
    static void
    add(std::uint64_t& destination, std::uint64_t increment, bool& did_saturate) noexcept {
        const auto sum = bioseq::common::checked_add(destination, increment);
        if (!sum) {
            destination = (std::numeric_limits<std::uint64_t>::max)();
            did_saturate = true;
            return;
        }
        destination = *sum;
    }

    /**
     * Check that the saved spill snapshot, algorithm identity and  execution counts
     * agree. Require work from the chosen backend when the target is long enough
     * to contain a full minimizer window.
     */
    [[nodiscard]] bool is_spill_valid() const noexcept {
        if (!route_snapshot_matches_() || !spill_complete || !backend_bound || saturated ||
            window_size == 0 || kmer_size == 0 || configured_core_span_bp == 0 ||
            peak_core_span_bp > configured_core_span_bp ||
            !is_valid_minimizer_generation_algorithm_identity(
                algorithm_identity,
                minimizer_producer_backend_identity_for_backend(selected_backend)) ||
            algorithm_identity.runtime_kmer_size != kmer_size ||
            algorithm_identity.runtime_window_size != window_size ||
            minimizer_scheme != minimizer_scheme_identity_for_backend(selected_backend) ||
            (spilled_event_count == 0) != (spill_run_count == 0) ||
            spill_qualification_scan_count != 1 ||
            spill_qualification_merge_count != (target_length == 0 ? 0u : 1u) ||
            authority_seal_ == 0 || spill_operation_epoch == 0 ||
            spill_operation_epoch != authority_operation_epoch_ ||
            spill_storage_binding != authority_storage_binding_ ||
            spilled_event_count != authority_event_count_ ||
            spill_content_digest != authority_content_digest_) {
            return false;
        }
        const bool no_exact = exact_simd_core_runs == 0 && !exact_simd_stats_available &&
                              !reference_simd_stats_available;
        const bool no_bounded = bounded_nthash_core_runs == 0 && !packed_bounded_stats_available;
        const bool no_vulkan = vulkan_core_runs == 0 && vulkan_stats.completed_roundtrips == 0 &&
                               vulkan_stats.downloaded_minimizers == 0 &&
                               vulkan_stats.gpu_window_splits == 0 && !vulkan_stats.saturated;
        const std::uint64_t minimum_work_bases =
            static_cast<std::uint64_t>(window_size) + kmer_size - 1u;
        const bool require_backend_work = target_length >= minimum_work_bases;
        if (!require_backend_work) {
            return scalar_core_runs == 0 && exact_simd_core_runs == 0 &&
                   bounded_nthash_core_runs == 0 && vulkan_core_runs == 0 && no_exact &&
                   no_bounded && no_vulkan &&
                   (target_length == 0 ? short_core_runs == 0 : short_core_runs != 0);
        }
        switch (selected_backend) {
        case minimizer_sketch_backend::scalar:
            return scalar_core_runs != 0 && exact_simd_core_runs == 0 &&
                   bounded_nthash_core_runs == 0 && vulkan_core_runs == 0 && no_exact &&
                   no_bounded && no_vulkan;
        case minimizer_sketch_backend::exact_simd:
            if (scalar_core_runs != 0 || exact_simd_core_runs == 0 ||
                bounded_nthash_core_runs != 0 || vulkan_core_runs != 0 || !no_bounded ||
                !no_vulkan) {
                return false;
            }
            if (algorithm_identity.effective_algorithm ==
                bioseq::simd::minimizer_algorithm::simd_minimap2_reference) {
                return !exact_simd_stats_available && reference_simd_stats_available &&
                       reference_simd_stats.is_internally_consistent(
                           true, algorithm_identity.packed_storage_bits / 8u);
            }
            return algorithm_identity.effective_algorithm ==
                       bioseq::simd::minimizer_algorithm::two_stack_nthash &&
                   exact_simd_stats_available && !reference_simd_stats_available &&
                   exact_simd_stats.is_internally_consistent(true);
        case minimizer_sketch_backend::bounded_nthash_simd:
            return scalar_core_runs == 0 && exact_simd_core_runs == 0 &&
                   bounded_nthash_core_runs != 0 && vulkan_core_runs == 0 && no_exact &&
                   no_vulkan && packed_bounded_stats_available &&
                   packed_bounded_status == packed_bounded_nthash_index_status::ok &&
                   packed_bounded_stats.is_internally_consistent(true);
        case minimizer_sketch_backend::vulkan_segment:
            return scalar_core_runs == 0 && exact_simd_core_runs == 0 &&
                   bounded_nthash_core_runs == 0 && vulkan_core_runs != 0 && no_exact &&
                   no_bounded && vulkan_stats.is_internally_consistent(true);
        }
        return false;
    }

    /**
     * Check the completed index against its spill, replay and publication records.
     * Successful replays must agree on event counts and digests. Failed planning
     * scans must be followed by a recorded segment split.
     */
    [[nodiscard]] bool is_internally_consistent() const noexcept {
        const std::uint64_t required_replays = required_successful_replay_count();
        if (!is_spill_valid() || !replay_snapshot_matches_() || !manifest_bound ||
            !manifest_authority_bound_ || !publication_binding_present_ ||
            !publication_binding_.valid() || generation_layout != publication_binding_.layout ||
            required_replays == 0u || manifest_position_count != spilled_event_count ||
            replay_illegal_span_transition_count != 0) {
            return false;
        }
        if (target_length == 0) {
            return replay_scan_count == required_replays && replay_merge_pass_count == 0 &&
                   replay_reroute_pass_count == 0 &&
                   replay_completed_scan_count == required_replays &&
                   replay_failed_scan_count == 0 && replay_binding_checked_failed_scan_count == 0 &&
                   replay_first_completed_event_count == 0 &&
                   replay_last_completed_event_count == 0 &&
                   replay_first_completed_content_digest == spill_content_digest &&
                   replay_last_completed_content_digest == spill_content_digest &&
                   replay_final_span_bp == 1 &&
                   replay_final_storage_binding == spill_storage_binding &&
                   manifest_position_segment_span_bp == 0;
        }
        return replay_reroute_pass_count <=
                   (std::numeric_limits<std::uint64_t>::max)() - required_replays &&
               replay_scan_count == replay_reroute_pass_count + required_replays &&
               replay_merge_pass_count == replay_scan_count &&
               replay_completed_scan_count == required_replays &&
               replay_failed_scan_count == replay_reroute_pass_count &&
               replay_binding_checked_failed_scan_count == replay_failed_scan_count &&
               replay_first_completed_event_count == spilled_event_count &&
               replay_last_completed_event_count == spilled_event_count &&
               replay_first_completed_content_digest == replay_last_completed_content_digest &&
               (replay_reroute_pass_count != 0 ||
                replay_first_completed_content_digest == spill_content_digest) &&
               replay_final_storage_binding.valid() &&
               replay_final_storage_binding.generation_token ==
                   spill_storage_binding.generation_token &&
               spill_storage_binding.mutation_revision <=
                   (std::numeric_limits<std::uint64_t>::max)() - replay_reroute_pass_count &&
               replay_final_storage_binding.mutation_revision ==
                   spill_storage_binding.mutation_revision + replay_reroute_pass_count &&
               replay_final_span_bp == manifest_position_segment_span_bp &&
               replay_final_span_bp != 0;
    }

    /** Check that this completed generation matches the expected publication identity. */
    [[nodiscard]] bool
    matches_publication(const minimizer_generation_publication_binding& expected) const noexcept {
        return is_internally_consistent() && expected.valid() && publication_binding_ == expected;
    }

    /** Check that the completed generation's spill data is still the version being released. */
    [[nodiscard]] bool
    allows_ready_release(direct_minimizer_spill_storage_binding before_release) const noexcept {
        return is_internally_consistent() && before_release.valid() &&
               before_release == replay_final_storage_binding &&
               before_release.generation_token == spill_storage_binding.generation_token;
    }

  private:
    template <class> friend class direct_minimizer_external_spill_replay_producer;
    template <class, class> friend struct direct_minimizer_generation_replay;

    // Copy these values with the receipt  so it remains checkable after the
    // shared spill handle has been destroyed.
    std::uint64_t authority_seal_ = 0;
    std::uint64_t authority_event_count_ = 0;
    direct_minimizer_content_digest authority_content_digest_{};
    std::uint64_t authority_operation_epoch_ = 0;
    direct_minimizer_spill_storage_binding authority_storage_binding_{};
    bool manifest_authority_bound_ = false;
    minimizer_generation_publication_binding publication_binding_{};
    bool publication_binding_present_ = false;

    /** Copy of the collection fields taken when the spill is checked. */
    struct route_snapshot {
        minimizer_sketch_backend selected_backend = minimizer_sketch_backend::scalar;
        minimizer_scheme_identity minimizer_scheme{};
        minimizer_generation_algorithm_identity algorithm_identity{};
        std::uint64_t target_length = 0;
        std::uint16_t window_size = 0;
        std::uint16_t kmer_size = 0;
        std::uint64_t configured_core_span_bp = 0;
        std::uint64_t peak_core_span_bp = 0;
        std::uint64_t peak_core_events = 0;
        std::uint64_t peak_bounded_materialized_window_bp = 0;
        std::uint64_t spill_run_count = 0;
        std::uint64_t spilled_event_count = 0;
        direct_minimizer_content_digest spill_content_digest{};
        std::uint64_t spill_operation_epoch = 0;
        direct_minimizer_spill_storage_binding spill_storage_binding{};
        std::uint64_t spill_qualification_scan_count = 0;
        std::uint64_t spill_qualification_merge_count = 0;
        std::uint64_t short_core_runs = 0;
        std::uint64_t scalar_core_runs = 0;
        std::uint64_t exact_simd_core_runs = 0;
        std::uint64_t bounded_nthash_core_runs = 0;
        std::uint64_t vulkan_core_runs = 0;
        exact_simd_index_execution_stats exact_simd_stats{};
        bool exact_simd_stats_available = false;
        reference_simd_index_execution_stats reference_simd_stats{};
        bool reference_simd_stats_available = false;
        packed_bounded_nthash_index_status packed_bounded_status =
            packed_bounded_nthash_index_status::invalid_state;
        packed_bounded_nthash_index_execution_stats packed_bounded_stats{};
        bool packed_bounded_stats_available = false;
        bioseq::gpu::vulkan::vulkan_segment_pair_execution_stats vulkan_stats{};
        bool spill_complete = false;
        bool backend_bound = false;
        bool saturated = false;
    } route_snapshot_{};
    bool route_snapshot_present_ = false;

    /** Copy of replay and manifest fields taken when the index is redy for publication */
    struct replay_snapshot {
        minimizer_generation_layout generation_layout = minimizer_generation_layout::invalid;
        std::uint64_t replay_scan_count = 0;
        std::uint64_t replay_merge_pass_count = 0;
        std::uint64_t replay_reroute_pass_count = 0;
        std::uint64_t replay_completed_scan_count = 0;
        std::uint64_t replay_failed_scan_count = 0;
        std::uint64_t replay_binding_checked_failed_scan_count = 0;
        std::uint64_t replay_illegal_span_transition_count = 0;
        std::uint64_t replay_first_completed_event_count = 0;
        std::uint64_t replay_last_completed_event_count = 0;
        direct_minimizer_content_digest replay_first_completed_content_digest{};
        direct_minimizer_content_digest replay_last_completed_content_digest{};
        std::uint64_t replay_final_span_bp = 0;
        direct_minimizer_spill_storage_binding replay_final_storage_binding{};
        std::uint64_t replay_peak_callback_batch_events = 0;
        std::uint64_t replay_visited_event_count = 0;
        std::uint64_t manifest_position_count = 0;
        std::uint64_t manifest_position_segment_span_bp = 0;
        bool manifest_bound = false;
        minimizer_generation_publication_binding publication_binding{};
        bool publication_binding_present = false;
    } replay_snapshot_{};
    bool replay_snapshot_present_ = false;

    /** Compare the Vulkan execution counts used to validate the spill. */
    [[nodiscard]] static bool same_vulkan_stats_(
        const bioseq::gpu::vulkan::vulkan_segment_pair_execution_stats& lhs,
        const bioseq::gpu::vulkan::vulkan_segment_pair_execution_stats& rhs) noexcept {
        return lhs.completed_roundtrips == rhs.completed_roundtrips &&
               lhs.downloaded_minimizers == rhs.downloaded_minimizers &&
               lhs.gpu_window_splits == rhs.gpu_window_splits && lhs.saturated == rhs.saturated;
    }

    /** Save the, current collection fields for later consistency checks. */
    void capture_route_snapshot_() noexcept {
        route_snapshot_ = route_snapshot{
            .selected_backend = selected_backend,
            .minimizer_scheme = minimizer_scheme,
            .algorithm_identity = algorithm_identity,
            .target_length = target_length,
            .window_size = window_size,
            .kmer_size = kmer_size,
            .configured_core_span_bp = configured_core_span_bp,
            .peak_core_span_bp = peak_core_span_bp,
            .peak_core_events = peak_core_events,
            .peak_bounded_materialized_window_bp = peak_bounded_materialized_window_bp,
            .spill_run_count = spill_run_count,
            .spilled_event_count = spilled_event_count,
            .spill_content_digest = spill_content_digest,
            .spill_operation_epoch = spill_operation_epoch,
            .spill_storage_binding = spill_storage_binding,
            .spill_qualification_scan_count = spill_qualification_scan_count,
            .spill_qualification_merge_count = spill_qualification_merge_count,
            .short_core_runs = short_core_runs,
            .scalar_core_runs = scalar_core_runs,
            .exact_simd_core_runs = exact_simd_core_runs,
            .bounded_nthash_core_runs = bounded_nthash_core_runs,
            .vulkan_core_runs = vulkan_core_runs,
            .exact_simd_stats = exact_simd_stats,
            .exact_simd_stats_available = exact_simd_stats_available,
            .reference_simd_stats = reference_simd_stats,
            .reference_simd_stats_available = reference_simd_stats_available,
            .packed_bounded_status = packed_bounded_status,
            .packed_bounded_stats = packed_bounded_stats,
            .packed_bounded_stats_available = packed_bounded_stats_available,
            .vulkan_stats = vulkan_stats,
            .spill_complete = spill_complete,
            .backend_bound = backend_bound,
            .saturated = saturated,
        };
        route_snapshot_present_ = true;
    }

    /** Check that the  collection fields still match their saved snapshot. */
    [[nodiscard]] bool route_snapshot_matches_() const noexcept {
        const route_snapshot& value = route_snapshot_;
        return route_snapshot_present_ && selected_backend == value.selected_backend &&
               minimizer_scheme == value.minimizer_scheme &&
               algorithm_identity == value.algorithm_identity &&
               target_length == value.target_length &&
               window_size == value.window_size && kmer_size == value.kmer_size &&
               configured_core_span_bp == value.configured_core_span_bp &&
               peak_core_span_bp == value.peak_core_span_bp &&
               peak_core_events == value.peak_core_events &&
               peak_bounded_materialized_window_bp == value.peak_bounded_materialized_window_bp &&
               spill_run_count == value.spill_run_count &&
               spilled_event_count == value.spilled_event_count &&
               spill_content_digest == value.spill_content_digest &&
               spill_operation_epoch == value.spill_operation_epoch &&
               spill_storage_binding == value.spill_storage_binding &&
               spill_qualification_scan_count == value.spill_qualification_scan_count &&
               spill_qualification_merge_count == value.spill_qualification_merge_count &&
               short_core_runs == value.short_core_runs &&
               scalar_core_runs == value.scalar_core_runs &&
               exact_simd_core_runs == value.exact_simd_core_runs &&
               bounded_nthash_core_runs == value.bounded_nthash_core_runs &&
               vulkan_core_runs == value.vulkan_core_runs &&
               exact_simd_stats == value.exact_simd_stats &&
               exact_simd_stats_available == value.exact_simd_stats_available &&
               reference_simd_stats == value.reference_simd_stats &&
               reference_simd_stats_available == value.reference_simd_stats_available &&
               packed_bounded_status == value.packed_bounded_status &&
               packed_bounded_stats == value.packed_bounded_stats &&
               packed_bounded_stats_available == value.packed_bounded_stats_available &&
               same_vulkan_stats_(vulkan_stats, value.vulkan_stats) &&
               spill_complete == value.spill_complete && backend_bound == value.backend_bound &&
               saturated == value.saturated;
    }

    /** Save the completed replay and manifest fields for later consistency checks. */
    void capture_replay_snapshot_() noexcept {
        replay_snapshot_ = replay_snapshot{
            .generation_layout = generation_layout,
            .replay_scan_count = replay_scan_count,
            .replay_merge_pass_count = replay_merge_pass_count,
            .replay_reroute_pass_count = replay_reroute_pass_count,
            .replay_completed_scan_count = replay_completed_scan_count,
            .replay_failed_scan_count = replay_failed_scan_count,
            .replay_binding_checked_failed_scan_count = replay_binding_checked_failed_scan_count,
            .replay_illegal_span_transition_count = replay_illegal_span_transition_count,
            .replay_first_completed_event_count = replay_first_completed_event_count,
            .replay_last_completed_event_count = replay_last_completed_event_count,
            .replay_first_completed_content_digest = replay_first_completed_content_digest,
            .replay_last_completed_content_digest = replay_last_completed_content_digest,
            .replay_final_span_bp = replay_final_span_bp,
            .replay_final_storage_binding = replay_final_storage_binding,
            .replay_peak_callback_batch_events = replay_peak_callback_batch_events,
            .replay_visited_event_count = replay_visited_event_count,
            .manifest_position_count = manifest_position_count,
            .manifest_position_segment_span_bp = manifest_position_segment_span_bp,
            .manifest_bound = manifest_bound,
            .publication_binding = publication_binding_,
            .publication_binding_present = publication_binding_present_,
        };
        replay_snapshot_present_ = true;
    }

    /** Check that replay and manifest fields still match their saved snapshot. */
    [[nodiscard]] bool replay_snapshot_matches_() const noexcept {
        const replay_snapshot& value = replay_snapshot_;
        return replay_snapshot_present_ && generation_layout == value.generation_layout &&
               replay_scan_count == value.replay_scan_count &&
               replay_merge_pass_count == value.replay_merge_pass_count &&
               replay_reroute_pass_count == value.replay_reroute_pass_count &&
               replay_completed_scan_count == value.replay_completed_scan_count &&
               replay_failed_scan_count == value.replay_failed_scan_count &&
               replay_binding_checked_failed_scan_count ==
                   value.replay_binding_checked_failed_scan_count &&
               replay_illegal_span_transition_count == value.replay_illegal_span_transition_count &&
               replay_first_completed_event_count == value.replay_first_completed_event_count &&
               replay_last_completed_event_count == value.replay_last_completed_event_count &&
               replay_first_completed_content_digest ==
                   value.replay_first_completed_content_digest &&
               replay_last_completed_content_digest == value.replay_last_completed_content_digest &&
               replay_final_span_bp == value.replay_final_span_bp &&
               replay_final_storage_binding == value.replay_final_storage_binding &&
               replay_peak_callback_batch_events == value.replay_peak_callback_batch_events &&
               replay_visited_event_count == value.replay_visited_event_count &&
               manifest_position_count == value.manifest_position_count &&
               manifest_position_segment_span_bp == value.manifest_position_segment_span_bp &&
               manifest_bound == value.manifest_bound &&
               publication_binding_ == value.publication_binding &&
               publication_binding_present_ == value.publication_binding_present;
    }

    /** Copy the checked spill identity into the receipt. */
    void bind_spill_authority(std::uint64_t seal,
                              std::uint64_t event_count,
                              const direct_minimizer_content_digest& content_digest,
                              direct_minimizer_spill_storage_binding storage_binding,
                              std::uint64_t operation_epoch) noexcept {
        authority_seal_ = seal;
        authority_event_count_ = event_count;
        authority_content_digest_ = content_digest;
        authority_storage_binding_ = storage_binding;
        authority_operation_epoch_ = operation_epoch;
    }

    /** Check that the supplied spill identity matches the receipt's private copy. */
    [[nodiscard]] bool authority_matches(std::uint64_t seal,
                                         std::uint64_t event_count,
                                         const direct_minimizer_content_digest& content_digest,
                                         direct_minimizer_spill_storage_binding storage_binding,
                                         std::uint64_t operation_epoch) const noexcept {
        return seal != 0 && seal == authority_seal_ && event_count == authority_event_count_ &&
               content_digest == authority_content_digest_ &&
               storage_binding == authority_storage_binding_ &&
               operation_epoch == authority_operation_epoch_;
    }

    /** Record that the manifest has been checked against the replayed events. */
    void bind_manifest_authority() noexcept {
        manifest_authority_bound_ = true;
    }

    /** Save the encoded generation's publication identity and check its algorithm  identity. */
    void bind_publication_authority(const encoded_grouped_soa_generation& generation) noexcept {
        publication_binding_ = make_minimizer_generation_publication_binding(generation);
        publication_binding_present_ = publication_binding_.valid() &&
                                       generation.manifest.algorithm_identity == algorithm_identity;
    }

    /** Issue the next nonzero spill-operation ID, or return zero after the counter is exhausted. */
    [[nodiscard]] static std::uint64_t next_spill_operation_epoch() noexcept {
        static std::atomic<std::uint64_t> epoch{1u};
        std::uint64_t value = epoch.load(std::memory_order_relaxed);
        for (;;) {
            if (value == 0)
                return 0;
            const std::uint64_t next =
                value == (std::numeric_limits<std::uint64_t>::max)() ? 0u : value + 1u;
            if (epoch.compare_exchange_weak(
                    value, next, std::memory_order_relaxed, std::memory_order_relaxed)) {
                return value;
            }
        }
    }

    /** Issue the next nonzero spill-handle ID, or return zero afer the counter is exhausted. */
    [[nodiscard]] static std::uint64_t next_spill_authority_seal() noexcept {
        static std::atomic<std::uint64_t> seal{1u};
        std::uint64_t value = seal.load(std::memory_order_relaxed);
        for (;;) {
            if (value == 0)
                return 0;
            const std::uint64_t next =
                value == (std::numeric_limits<std::uint64_t>::max)() ? 0u : value + 1u;
            if (seal.compare_exchange_weak(
                    value, next, std::memory_order_relaxed, std::memory_order_relaxed)) {
                return value;
            }
        }
    }
};

/// Private record of a checked spill, including its storage object, generation
/// token, event digest and algorithm. Only the generation builder can create it.
template <class p_storage> class direct_minimizer_spill_authority {
  private:
    template <class, class> friend struct direct_minimizer_generation_replay;
    template <class> friend class direct_minimizer_external_spill_replay_producer;

    p_storage* storage_ = nullptr;
    std::uint64_t target_length_ = 0;
    std::uint64_t initial_span_bp_ = 0;
    std::uint64_t run_count_ = 0;
    std::uint64_t event_count_ = 0;
    direct_minimizer_content_digest content_digest_{};
    direct_minimizer_spill_storage_binding storage_binding_{};
    std::uint64_t operation_epoch_ = 0;
    std::uint64_t authority_seal_ = 0;
    minimizer_scheme_identity minimizer_scheme_{};
    minimizer_generation_algorithm_identity algorithm_identity_{};

    /** Save the spill identity established by the completed validation scan. */
    direct_minimizer_spill_authority(p_storage& storage,
                                     std::uint64_t target_length,
                                     std::uint64_t initial_span_bp,
                                     std::uint64_t run_count,
                                     std::uint64_t event_count,
                                     const direct_minimizer_content_digest& content_digest,
                                     direct_minimizer_spill_storage_binding storage_binding,
                                     std::uint64_t operation_epoch,
                                     std::uint64_t authority_seal,
                                     minimizer_scheme_identity minimizer_scheme,
                                     minimizer_generation_algorithm_identity algorithm_identity)
        noexcept
        : storage_(&storage), target_length_(target_length), initial_span_bp_(initial_span_bp),
          run_count_(run_count), event_count_(event_count), content_digest_(content_digest),
          storage_binding_(storage_binding), operation_epoch_(operation_epoch),
          authority_seal_(authority_seal), minimizer_scheme_(minimizer_scheme),
          algorithm_identity_(algorithm_identity) {}

  public:
    /** Copying is disabled to keep  one identity for this checked spill. */
    direct_minimizer_spill_authority(const direct_minimizer_spill_authority&) = delete;
    /** Copy assignment is disabled to keep the checked spill identity, unchanged */
    direct_minimizer_spill_authority& operator=(const direct_minimizer_spill_authority&) = delete;
    /** Destroy the identity record. Spill data remains owned by storage. */
    ~direct_minimizer_spill_authority() = default;
};

/// Feeds stored minimizer events to the index encoder in segment/hash/position
/// order. Storage owns the merge buffers and runs. This producer checks the
/// storage version, event count and SHA-256 digest on each scan. A failed
/// planning pass may retry with half-sized position segments.
template <class p_storage> class direct_minimizer_external_spill_replay_producer {
  private:
    p_storage* storage_ = nullptr;
    minimizer_scheme_identity minimizer_scheme_{};
    minimizer_generation_algorithm_identity algorithm_identity_{};
    std::uint64_t target_length_ = 0;
    std::uint64_t qualified_span_ = 0;
    minimizer_generation_layout generation_layout_ = minimizer_generation_layout::invalid;
    std::uint64_t scan_count_ = 0;
    std::uint64_t merge_pass_count_ = 0;
    std::uint64_t reroute_pass_count_ = 0;
    std::uint64_t peak_callback_batch_events_ = 0;
    std::uint64_t visited_event_count_ = 0;
    std::uint64_t completed_scan_count_ = 0;
    std::uint64_t failed_scan_count_ = 0;
    std::uint64_t illegal_span_transition_count_ = 0;
    std::uint64_t first_completed_event_count_ = 0;
    std::uint64_t last_completed_event_count_ = 0;
    direct_minimizer_content_digest first_completed_content_digest_{};
    direct_minimizer_content_digest last_completed_content_digest_{};
    std::uint64_t final_span_bp_ = 0;
    direct_minimizer_spill_storage_binding storage_binding_{};
    direct_minimizer_content_digest expected_replay_digest_{};
    bool expected_replay_digest_valid_ = false;
    std::uint64_t binding_checked_failed_scan_count_ = 0;
    bool has_completed_scan_ = false;
    bool saturated_ = false;
    bool terminal_ = false;
    bool requires_halving_after_failed_scan_ = false;
    minimizer_sequence_generation_execution_receipt spill_receipt_{};
    std::shared_ptr<const direct_minimizer_spill_authority<p_storage>> authority_{};

    /** Add to a replay counter and fuag overflow. */
    void add_counter_(std::uint64_t& destination, std::uint64_t increment = 1u) noexcept {
        minimizer_sequence_generation_execution_receipt::add(destination, increment, saturated_);
    }

    /** Return three completed scans for  grouped-SoA, or two before layout selection. */
    [[nodiscard]] std::uint64_t required_successful_replay_count_() const noexcept {
        if (generation_layout_ == minimizer_generation_layout::grouped_soa_segment_local) {
            return 3u;
        }
        return generation_layout_ == minimizer_generation_layout::invalid ||
                       is_supported_minimizer_generation_layout(generation_layout_)
                   ? 2u
                   : 0u;
    }

    /** Record a completed scan, saving the first and latest event counts and digests. */
    void record_completed_scan_(std::uint64_t event_count,
                                const direct_minimizer_content_digest& content_digest,
                                std::uint64_t span) noexcept {
        add_counter_(completed_scan_count_);
        if (!has_completed_scan_) {
            first_completed_event_count_ = event_count;
            first_completed_content_digest_ = content_digest;
            has_completed_scan_ = true;
        }
        last_completed_event_count_ = event_count;
        last_completed_content_digest_ = content_digest;
        final_span_bp_ = span;
    }

    /** Count a failed scan and return its error. */
    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    fail_scan_(minimizer_generation_encoding_error error) noexcept {
        add_counter_(failed_scan_count_);
        return std::unexpected(error);
    }

    template <class, class> friend struct direct_minimizer_generation_replay;
    template <class> friend class direct_minimizer_unbound_external_spill_replay_producer;

    /** Attach the spill storage and settings, plus a checked spill handle when available. */
    direct_minimizer_external_spill_replay_producer(
        p_storage& storage,
        std::uint64_t target_length,
        std::uint64_t qualified_span,
        minimizer_scheme_identity minimizer_scheme,
        const minimizer_sequence_generation_execution_receipt& spill_receipt = {},
        std::shared_ptr<const direct_minimizer_spill_authority<p_storage>> authority = {},
        direct_minimizer_spill_storage_binding storage_binding = {}) noexcept
        : storage_(&storage), minimizer_scheme_(minimizer_scheme), target_length_(target_length),
          qualified_span_(qualified_span), spill_receipt_(spill_receipt),
          authority_(std::move(authority)), storage_binding_(storage_binding) {
        algorithm_identity_ = spill_receipt_.algorithm_identity;
        if (!storage_binding_.valid()) {
            storage_binding_ = storage.segment_spill_storage_binding();
        }
        if (authority_ != nullptr) {
            expected_replay_digest_ = authority_->content_digest_;
            expected_replay_digest_valid_ = true;
        }
    }

  public:
    /** Copying is disabled because scan progress belongs to one producer. */
    direct_minimizer_external_spill_replay_producer(
        const direct_minimizer_external_spill_replay_producer&) = delete;
    /** Copy assignment is disabled because scan progress belongs to one producer. */
    direct_minimizer_external_spill_replay_producer&
    operator=(const direct_minimizer_external_spill_replay_producer&) = delete;
    /** Moving is disabled so the active producer  stays at one address. */
    direct_minimizer_external_spill_replay_producer(
        direct_minimizer_external_spill_replay_producer&&) = delete;
    /** Move assignment is disabled for an active replay producer. */
    direct_minimizer_external_spill_replay_producer&
    operator=(direct_minimizer_external_spill_replay_producer&&) = delete;

    /** Select the saved layout before the first scan. An invalid change disables this producer. */
    void bind_generation_layout(minimizer_generation_layout layout) noexcept {
        if (!is_supported_minimizer_generation_layout(layout) || scan_count_ != 0u ||
            (generation_layout_ != minimizer_generation_layout::invalid &&
             generation_layout_ != layout)) {
            saturated_ = true;
            terminal_ = true;
            return;
        }
        generation_layout_ = layout;
    }

    /** Return the hash and selection scheme supplied at construction. */
    [[nodiscard]] minimizer_scheme_identity scheme_identity() const noexcept {
        return minimizer_scheme_;
    }

    /// Return the implementation recorded by a vulid spill receipt
    /// Without that receipt, return an invalid producer  identity.
    [[nodiscard]] minimizer_producer_identity producer_backend_identity() const noexcept {
        if (!spill_receipt_.is_spill_valid()) {
            return {};
        }
        const minimizer_producer_identity identity =
            minimizer_producer_backend_identity_for_backend(spill_receipt_.selected_backend);
        return minimizer_scheme_identity_for_producer_backend(identity) == minimizer_scheme_
                   ? identity
                   : minimizer_producer_identity{};
    }

    /// Return the algorithm identity copied from the spill receipt.
    [[nodiscard]] minimizer_generation_algorithm_identity algorithm_identity() const noexcept {
        return algorithm_identity_;
    }

    /**
     * Replay the merged events and check their count, digest and storage version.
     * Continue checking the stream if the receiver reports an error. Before any
     * successful scan, that error permits a retry with half the segment span.
     * Invalid data or changes to the spill disable further replay.
     */
    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    scan_sorted_events(std::uint64_t persisted_span, canonical_minimizer_event_receiver receiver) {
        add_counter_(scan_count_);
        if (terminal_ || storage_ == nullptr || persisted_span == 0) {
            return fail_scan_(minimizer_generation_encoding_error::segment_span);
        }
        if (!storage_binding_.valid() ||
            storage_->segment_spill_storage_binding() != storage_binding_) {
            terminal_ = true;
            return fail_scan_(minimizer_generation_encoding_error::replay_mismatch);
        }
        if (target_length_ == 0) {
            bioseq::common::sha256_hasher empty_hasher;
            const direct_minimizer_content_digest empty_digest = empty_hasher.finalize();
            if (storage_->segment_spill_storage_binding() != storage_binding_) {
                terminal_ = true;
                return fail_scan_(minimizer_generation_encoding_error::replay_mismatch);
            }
            if (authority_ != nullptr &&
                (authority_->storage_ != storage_ || authority_->target_length_ != target_length_ ||
                 authority_->event_count_ != 0 || authority_->content_digest_ != empty_digest ||
                 authority_->operation_epoch_ == 0 ||
                 authority_->storage_binding_ != storage_binding_ ||
                 authority_->minimizer_scheme_ != minimizer_scheme_ ||
                 authority_->algorithm_identity_ != algorithm_identity_ ||
                 !spill_receipt_.authority_matches(authority_->authority_seal_,
                                                   0,
                                                   empty_digest,
                                                   authority_->storage_binding_,
                                                   authority_->operation_epoch_))) {
                terminal_ = true;
                return fail_scan_(minimizer_generation_encoding_error::replay_mismatch);
            }
            record_completed_scan_(0, empty_digest, persisted_span);
            if (completed_scan_count_ == required_successful_replay_count_()) {
                terminal_ = true;
            }
            return {};
        }
        if (persisted_span != qualified_span_) {
            const std::uint64_t expected_next_span = qualified_span_ / 2u;
            if (!requires_halving_after_failed_scan_ || qualified_span_ <= 1u ||
                persisted_span != expected_next_span) {
                add_counter_(illegal_span_transition_count_);
                terminal_ = true;
                return fail_scan_(minimizer_generation_encoding_error::segment_span);
            }
            const direct_minimizer_spill_storage_binding before_reroute = storage_binding_;
            if (reroute_pass_count_ == (std::numeric_limits<std::uint64_t>::max)() ||
                !storage_->reroute_segment_runs(qualified_span_, persisted_span, target_length_)) {
                terminal_ = true;
                return fail_scan_(minimizer_generation_encoding_error::position_range);
            }
            const direct_minimizer_spill_storage_binding after_reroute =
                storage_->segment_spill_storage_binding();
            if (!after_reroute.valid() ||
                after_reroute.generation_token != before_reroute.generation_token ||
                before_reroute.mutation_revision == (std::numeric_limits<std::uint64_t>::max)() ||
                after_reroute.mutation_revision != before_reroute.mutation_revision + 1u) {
                terminal_ = true;
                return fail_scan_(minimizer_generation_encoding_error::replay_mismatch);
            }
            add_counter_(reroute_pass_count_);
            qualified_span_ = persisted_span;
            storage_binding_ = after_reroute;
            expected_replay_digest_valid_ = false;
            requires_halving_after_failed_scan_ = false;
        } else if (requires_halving_after_failed_scan_) {
            add_counter_(illegal_span_transition_count_);
            terminal_ = true;
            return fail_scan_(minimizer_generation_encoding_error::segment_span);
        }
        const std::uint64_t segment_count =
            target_length_ / persisted_span + (target_length_ % persisted_span != 0 ? 1u : 0u);
        if (!sql_integer_can_represent(segment_count)) {
            terminal_ = true;
            return fail_scan_(minimizer_generation_encoding_error::segment_count);
        }
        if (merge_pass_count_ == (std::numeric_limits<std::uint64_t>::max)()) {
            terminal_ = true;
            return fail_scan_(minimizer_generation_encoding_error::arithmetic_overflow);
        }
        add_counter_(merge_pass_count_);
        std::optional<minimizer_generation_encoding_error> structural_error;
        std::optional<minimizer_generation_encoding_error> receiver_error;
        std::uint64_t scan_event_count = 0;
        bioseq::common::sha256_hasher scan_content_hasher;
        std::uint32_t previous_segment = 0;
        bool have_segment = false;
        storage_->merge_segment_runs(
            [this,
             &structural_error,
             &receiver_error,
             &scan_event_count,
             &scan_content_hasher,
             &previous_segment,
             &have_segment,
             &segment_count,
             &persisted_span,
             &receiver](std::uint32_t segment, std::uint64_t hash, auto&& positions) {
                if (structural_error) {
                    return;
                }
                if (segment >= segment_count || (have_segment && segment < previous_segment)) {
                    structural_error = minimizer_generation_encoding_error::segment_ordinal;
                    return;
                }
                have_segment = true;
                previous_segment = segment;
                positions.for_each_batch([this,
                                          &structural_error,
                                          &receiver_error,
                                          &scan_event_count,
                                          &scan_content_hasher,
                                          &hash,
                                          &persisted_span,
                                          &segment,
                                          &receiver](std::span<const std::uint64_t> batch) {
                    peak_callback_batch_events_ =
                        std::max<std::uint64_t>(peak_callback_batch_events_, batch.size());
                    if (structural_error) {
                        return;
                    }
                    for (const std::uint64_t encoded_position : batch) {
                        const std::uint64_t position = encoded_position >> 1u;
                        if (position >= target_length_ || position / persisted_span != segment) {
                            structural_error = minimizer_generation_encoding_error::position_range;
                            return;
                        }
                        if (visited_event_count_ == (std::numeric_limits<std::uint64_t>::max)()) {
                            structural_error =
                                minimizer_generation_encoding_error::arithmetic_overflow;
                            return;
                        }
                        add_counter_(visited_event_count_);
                        add_counter_(scan_event_count);
                        scan_content_hasher.update_little_endian(hash);
                        scan_content_hasher.update_little_endian(encoded_position);
                        if (!receiver_error) {
                            if (auto accepted = receiver(canonical_minimizer_event{
                                    .hash = hash,
                                    .encoded_position = encoded_position,
                                });
                                !accepted) {
                                receiver_error = accepted.error();
                            }
                        }
                    }
                });
            });
        if (storage_->segment_spill_storage_binding() != storage_binding_) {
            structural_error = minimizer_generation_encoding_error::replay_mismatch;
        }
        if (structural_error) {
            terminal_ = true;
            return fail_scan_(*structural_error);
        }
        const direct_minimizer_content_digest content_digest = scan_content_hasher.finalize();
        if (authority_ != nullptr &&
            (authority_->storage_ != storage_ || authority_->target_length_ != target_length_ ||
             authority_->event_count_ != scan_event_count || authority_->operation_epoch_ == 0 ||
             authority_->minimizer_scheme_ != minimizer_scheme_ ||
             authority_->algorithm_identity_ != algorithm_identity_ ||
             !spill_receipt_.authority_matches(authority_->authority_seal_,
                                               authority_->event_count_,
                                               authority_->content_digest_,
                                               authority_->storage_binding_,
                                               authority_->operation_epoch_))) {
            terminal_ = true;
            return fail_scan_(minimizer_generation_encoding_error::replay_mismatch);
        }
        if (expected_replay_digest_valid_) {
            if (content_digest != expected_replay_digest_) {
                terminal_ = true;
                return fail_scan_(minimizer_generation_encoding_error::replay_mismatch);
            }
        } else {
            expected_replay_digest_ = content_digest;
            expected_replay_digest_valid_ = true;
        }
        if (receiver_error) {
            add_counter_(binding_checked_failed_scan_count_);
            if (completed_scan_count_ != 0) {
                terminal_ = true;
            } else {
                requires_halving_after_failed_scan_ = true;
            }
            return fail_scan_(*receiver_error);
        }
        record_completed_scan_(scan_event_count, content_digest, persisted_span);
        if (completed_scan_count_ == required_successful_replay_count_()) {
            terminal_ = true;
        }
        return {};
    }

    /** @brief Returns the number of replay scans attempted so far. */
    [[nodiscard]] std::uint64_t scan_count() const noexcept {
        return scan_count_;
    }
    /** @brief Returns the number of storage merge passes executed during replay. */
    [[nodiscard]] std::uint64_t merge_pass_count() const noexcept {
        return merge_pass_count_;
    }
    /** Return  the, number of retries that split position segments in half. */
    [[nodiscard]] std::uint64_t reroute_pass_count() const noexcept {
        return reroute_pass_count_;
    }
    /** @brief Returns the largest merge callback batch observed during replay. */
    [[nodiscard]] std::uint64_t peak_callback_batch_events() const noexcept {
        return peak_callback_batch_events_;
    }
    /** Return the number of events examined across all scans, including failed scans. */
    [[nodiscard]] std::uint64_t visited_event_count() const noexcept {
        return visited_event_count_;
    }
    /** @brief Returns the number of replay scans that completed successfully. */
    [[nodiscard]] std::uint64_t completed_scan_count() const noexcept {
        return completed_scan_count_;
    }
    /** @brief Returns the number of replay scans that failed. */
    [[nodiscard]] std::uint64_t failed_scan_count() const noexcept {
        return failed_scan_count_;
    }
    /** Return the number of rejected changes to the segment span. */
    [[nodiscard]] std::uint64_t illegal_span_transition_count() const noexcept {
        return illegal_span_transition_count_;
    }
    /** Return the spill execution record with the replay results recorded so far. */
    [[nodiscard]] minimizer_sequence_generation_execution_receipt
    execution_receipt() const noexcept {
        minimizer_sequence_generation_execution_receipt receipt = spill_receipt_;
        receipt.generation_layout = generation_layout_;
        receipt.replay_scan_count = scan_count_;
        receipt.replay_merge_pass_count = merge_pass_count_;
        receipt.replay_reroute_pass_count = reroute_pass_count_;
        receipt.replay_completed_scan_count = completed_scan_count_;
        receipt.replay_failed_scan_count = failed_scan_count_;
        receipt.replay_binding_checked_failed_scan_count = binding_checked_failed_scan_count_;
        receipt.replay_illegal_span_transition_count = illegal_span_transition_count_;
        receipt.replay_first_completed_event_count = first_completed_event_count_;
        receipt.replay_last_completed_event_count = last_completed_event_count_;
        receipt.replay_first_completed_content_digest = first_completed_content_digest_;
        receipt.replay_last_completed_content_digest = last_completed_content_digest_;
        receipt.replay_final_span_bp = final_span_bp_;
        receipt.replay_final_storage_binding = storage_binding_;
        receipt.replay_peak_callback_batch_events = peak_callback_batch_events_;
        receipt.replay_visited_event_count = visited_event_count_;
        receipt.saturated = receipt.saturated || saturated_;
        return receipt;
    }
    /** Return zero because the spill store owns the event buffers. */
    [[nodiscard]] static constexpr std::uint64_t owned_event_capacity() noexcept {
        return 0;
    }
};

/// Replays spill data before it has a checked generation receipt. The builder
/// uses this for its first validation scan, and tests use it to exercise retries.
/// It cannot produce a valid completed-generation  receipt.
template <class p_storage> class direct_minimizer_unbound_external_spill_replay_producer {
  private:
    /** Use the current spill-generation token, staroing a generation if none exists. */
    static direct_minimizer_spill_storage_binding binding_for_(p_storage& storage) noexcept {
        direct_minimizer_spill_storage_binding binding = storage.segment_spill_storage_binding();
        if (!binding.valid()) {
            binding = storage.begin_segment_spill_generation();
        }
        return binding;
    }

    direct_minimizer_external_spill_replay_producer<p_storage> producer_;

  public:
    /** Attach spill storage without a checked generation receipt. */
    direct_minimizer_unbound_external_spill_replay_producer(
        p_storage& storage,
        std::uint64_t target_length,
        std::uint64_t qualified_span,
        minimizer_scheme_identity minimizer_scheme) noexcept
        : producer_(storage,
                    target_length,
                    qualified_span,
                    minimizer_scheme,
                    {},
                    {},
                    binding_for_(storage)) {}

    /** Copying is disabled because scan progress belongs to one adapter */
    direct_minimizer_unbound_external_spill_replay_producer(
        const direct_minimizer_unbound_external_spill_replay_producer&) = delete;
    /** Copy assignment is disabled because scan progress belongs to one adapter. */
    direct_minimizer_unbound_external_spill_replay_producer&
    operator=(const direct_minimizer_unbound_external_spill_replay_producer&) = delete;
    /** Moving is disabled with the wrapped replay producer. */
    direct_minimizer_unbound_external_spill_replay_producer(
        direct_minimizer_unbound_external_spill_replay_producer&&) = delete;
    /** Move assignment is disabled with the wrapped  replay producer. */
    direct_minimizer_unbound_external_spill_replay_producer&
    operator=(direct_minimizer_unbound_external_spill_replay_producer&&) = delete;

    /** Select the layout on the wrapped replay producer. */
    void bind_generation_layout(minimizer_generation_layout layout) noexcept {
        producer_.bind_generation_layout(layout);
    }

    /** Return the hash and selection scheme used for this replay. */
    [[nodiscard]] minimizer_scheme_identity scheme_identity() const noexcept {
        return producer_.scheme_identity();
    }
    /** Return an invalid producer identity because this adapter has no checked spill receipt. */
    [[nodiscard]] minimizer_producer_identity producer_backend_identity() const noexcept {
        return producer_.producer_backend_identity();
    }
    /** Return the default algorithm identity of this unchecked replay adapter. */
    [[nodiscard]] minimizer_generation_algorithm_identity algorithm_identity() const noexcept {
        return producer_.algorithm_identity();
    }
    /** Replay one scan through the wrapped producer. */
    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    scan_sorted_events(std::uint64_t persisted_span, canonical_minimizer_event_receiver receiver) {
        return producer_.scan_sorted_events(persisted_span, receiver);
    }
    /** Return the number of  scan attempts. */
    [[nodiscard]] std::uint64_t scan_count() const noexcept {
        return producer_.scan_count();
    }
    /** Return the number of storage merge passes. */
    [[nodiscard]] std::uint64_t merge_pass_count() const noexcept {
        return producer_.merge_pass_count();
    }
    /** Return the number of, segment-hplitting retries. */
    [[nodiscard]] std::uint64_t reroute_pass_count() const noexcept {
        return producer_.reroute_pass_count();
    }
    /** Return the number of completed scans. */
    [[nodiscard]] std::uint64_t completed_scan_count() const noexcept {
        return producer_.completed_scan_count();
    }
    /** Return the number of failed scans. */
    [[nodiscard]] std::uint64_t failed_scan_count() const noexcept {
        return producer_.failed_scan_count();
    }
    /** Return the number of rejected segment-span changes. */
    [[nodiscard]] std::uint64_t illegal_span_transition_count() const noexcept {
        return producer_.illegal_span_transition_count();
    }
    /** Return the replay statistics collected so far. */
    [[nodiscard]] minimizer_sequence_generation_execution_receipt
    execution_receipt() const noexcept {
        return producer_.execution_receipt();
    }
    /** Return zero because the spill store owns the event buffers. */
    [[nodiscard]] static constexpr std::uint64_t owned_event_capacity() noexcept {
        return 0;
    }
};

} // namespace bioseq::indexing
