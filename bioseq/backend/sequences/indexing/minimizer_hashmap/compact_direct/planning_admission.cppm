/**
 * @file compact_direct/planning_admission.cppm
 * @brief Compact-direct planning state and admitted anchor ownership.
 */

export module bioseq.indexing.minimizer_hashmap:compact_direct.planning_admission;

import :compact_direct.query_contracts;
import std;
import :exact_simd;
import :grouped_soa;
import :prepared_reader;
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

/// Reusable, separately admitted query-wave planning state. The scratch owns
/// only transient planner/layout capacity and is never referenced by a
/// returned anchor span.
class query_wave_scratch {
  private:
    static constexpr std::size_t query_membership_filter_word_bits_ = 21u;
    static constexpr std::size_t query_membership_filter_word_count_ =
        std::size_t{1u} << query_membership_filter_word_bits_;

    enum class wave_state : std::uint8_t {
        idle,
        preparing,
        outstanding,
        cleaning,
    };

    struct query_hash_plan {
        std::uint32_t hash = 0u;
        std::uint32_t occurrence_ref_begin = 0u;
        std::uint32_t occurrence_ref_count = 0u;
        std::uint32_t global_occurrence_count = 0u;
    };
    static_assert(sizeof(query_hash_plan) == 16u);

    // Declared first so all owned capacities die before their permit.
    bioseq::parallelism::memory_permit permit_{};
    std::vector<packed_query_occurrence> packed_occurrences_{};
    std::vector<wide_query_occurrence> wide_occurrences_{};
    std::vector<query_hash_plan> query_plans_{};
    std::vector<std::uint32_t> query_group_offsets_{};
    std::vector<std::uint32_t> query_plan_radix_{};
    std::unique_ptr<std::uint64_t[]> query_membership_filter_{};
    std::size_t query_membership_filter_word_capacity_ = 0u;
    std::unique_ptr<std::atomic<std::uint32_t>[]> occurrence_counts_{};
    std::size_t occurrence_count_capacity_ = 0u;
    std::size_t active_occurrence_count_ = 0u;
    std::vector<block_query_anchor_layout> block_query_layouts_{};
    std::vector<fragment_block_wave_stats> block_stats_{};
    query_occurrence_packing_layout occurrence_layout_{};
    std::size_t query_plan_reserve_request_ = 0u;
    bool occurrence_radix_invoked_ = false;
    bool parallel_occurrence_radix_split_selected_ = false;
    bool bucket_direct_matcher_active_ = false;
    bioseq::parallelism::memory_admission_controller* admission_ = nullptr;
    std::atomic<wave_state> wave_state_{wave_state::idle};

    friend class prepared_minimizer_fragment_reader;

    query_wave_scratch() = default;

    [[nodiscard]] std::uint64_t retained_occurrence_capacity_bytes_() const noexcept {
        using bioseq::common::saturating_add;
        using bioseq::common::saturating_mul;
        return saturating_add(
            saturating_mul(packed_occurrences_.capacity(), sizeof(packed_query_occurrence)),
            saturating_mul(wide_occurrences_.capacity(), sizeof(wide_query_occurrence)));
    }

    [[nodiscard]] std::uint64_t retained_capacity_bytes_() const noexcept {
        using bioseq::common::saturating_add;
        using bioseq::common::saturating_mul;
        std::uint64_t bytes = retained_occurrence_capacity_bytes_();
        bytes =
            saturating_add(bytes, saturating_mul(query_plans_.capacity(), sizeof(query_hash_plan)));
        bytes = saturating_add(
            bytes, saturating_mul(query_group_offsets_.capacity(), sizeof(std::uint32_t)));
        bytes = saturating_add(bytes,
                               saturating_mul(query_plan_radix_.capacity(), sizeof(std::uint32_t)));
        bytes = saturating_add(
            bytes, saturating_mul(query_membership_filter_word_capacity_, sizeof(std::uint64_t)));
        bytes = saturating_add(
            bytes, saturating_mul(occurrence_count_capacity_, sizeof(std::atomic<std::uint32_t>)));
        bytes = saturating_add(
            bytes,
            saturating_mul(block_query_layouts_.capacity(), sizeof(block_query_anchor_layout)));
        bytes = saturating_add(
            bytes, saturating_mul(block_stats_.capacity(), sizeof(fragment_block_wave_stats)));
        return bytes;
    }

    void release_all_capacity_noexcept_() noexcept {
        {
            std::vector<packed_query_occurrence> empty_packed_occurrences;
            std::vector<wide_query_occurrence> empty_wide_occurrences;
            std::vector<query_hash_plan> empty_plans;
            std::vector<std::uint32_t> empty_group_offsets;
            std::vector<std::uint32_t> empty_radix;
            std::vector<block_query_anchor_layout> empty_layouts;
            std::vector<fragment_block_wave_stats> empty_block_stats;
            packed_occurrences_.swap(empty_packed_occurrences);
            wide_occurrences_.swap(empty_wide_occurrences);
            query_plans_.swap(empty_plans);
            query_group_offsets_.swap(empty_group_offsets);
            query_plan_radix_.swap(empty_radix);
            block_query_layouts_.swap(empty_layouts);
            block_stats_.swap(empty_block_stats);
            occurrence_counts_.reset();
            query_membership_filter_.reset();
        }
        query_membership_filter_word_capacity_ = 0u;
        occurrence_count_capacity_ = 0u;
        active_occurrence_count_ = 0u;
        occurrence_layout_ = {};
        query_plan_reserve_request_ = 0u;
        occurrence_radix_invoked_ = false;
        parallel_occurrence_radix_split_selected_ = false;
        bucket_direct_matcher_active_ = false;
        permit_.shrink_to(0u);
    }

    void ensure_query_membership_filter_capacity_() {
        using bioseq::common::saturating_add;
        using bioseq::common::saturating_mul;
        if (query_membership_filter_word_capacity_ == query_membership_filter_word_count_) {
            std::fill_n(query_membership_filter_.get(),
                        query_membership_filter_word_capacity_,
                        std::uint64_t{0u});
            return;
        }
        const std::uint64_t retained = retained_capacity_bytes_();
        const std::uint64_t allocation =
            saturating_mul(query_membership_filter_word_count_, sizeof(std::uint64_t));
        const std::uint64_t peak = saturating_add(retained, allocation);
        if (allocation == (std::numeric_limits<std::uint64_t>::max)() ||
            peak == (std::numeric_limits<std::uint64_t>::max)() || !permit_.grow_to(peak)) {
            throw persisted_minimizer_lookup_error(
                "fragment-native query membership filter admission failed");
        }
        std::unique_ptr<std::uint64_t[]> replacement;
        try {
            replacement = std::make_unique_for_overwrite<std::uint64_t[]>(
                query_membership_filter_word_count_);
        } catch (const std::bad_alloc&) {
            permit_.shrink_to(retained_capacity_bytes_());
            throw persisted_minimizer_lookup_error(
                "fragment-native query membership filter allocation failed");
        }
        query_membership_filter_ = std::move(replacement);
        query_membership_filter_word_capacity_ = query_membership_filter_word_count_;
        std::fill_n(query_membership_filter_.get(),
                    query_membership_filter_word_capacity_,
                    std::uint64_t{0u});
        const std::uint64_t actual = retained_capacity_bytes_();
        if (actual == (std::numeric_limits<std::uint64_t>::max)() || actual > permit_.bytes()) {
            release_all_capacity_noexcept_();
            throw persisted_minimizer_lookup_error(
                "fragment-native query membership filter reconciliation failed");
        }
        permit_.shrink_to(actual);
    }

    template <class p_value>
    void ensure_vector_capacity_(std::vector<p_value>& values, std::size_t required_capacity) {
        if (required_capacity <= values.capacity())
            return;
        if (required_capacity > values.max_size()) {
            throw persisted_minimizer_lookup_error(
                "fragment-native query scratch capacity is out of range");
        }
        using bioseq::common::saturating_add;
        using bioseq::common::saturating_mul;
        const std::uint64_t retained = retained_capacity_bytes_();
        const std::uint64_t requested = saturating_mul(required_capacity, sizeof(p_value));
        const std::uint64_t headroom =
            saturating_mul(requested, bioseq::parallelism::k_vector_preallocation_headroom_factor);
        const std::uint64_t peak = saturating_add(retained, headroom);
        if (requested == (std::numeric_limits<std::uint64_t>::max)() ||
            headroom == (std::numeric_limits<std::uint64_t>::max)() ||
            peak == (std::numeric_limits<std::uint64_t>::max)()) {
            throw persisted_minimizer_lookup_error(
                "fragment-native query scratch admission size overflow");
        }
        if (!permit_.grow_to(peak)) {
            throw persisted_minimizer_lookup_error(
                persisted_minimizer_lookup_error::reason::admission_or_allocation,
                "fragment-native query scratch admission failed");
        }
        try {
            values.reserve(required_capacity);
        } catch (const std::bad_alloc&) {
            permit_.shrink_to(retained_capacity_bytes_());
            throw persisted_minimizer_lookup_error(
                persisted_minimizer_lookup_error::reason::admission_or_allocation,
                "fragment-native query scratch allocation failed");
        }
        const std::uint64_t actual = retained_capacity_bytes_();
        if (actual == (std::numeric_limits<std::uint64_t>::max)()) {
            release_all_capacity_noexcept_();
            throw persisted_minimizer_lookup_error(
                "fragment-native query scratch reconciliation overflow");
        }
        if (actual > permit_.bytes() && !permit_.grow_to(actual)) {
            release_all_capacity_noexcept_();
            throw persisted_minimizer_lookup_error(
                persisted_minimizer_lookup_error::reason::admission_or_allocation,
                "fragment-native query scratch reconciliation admission "
                "failed");
        }
        permit_.shrink_to(actual);
    }

    void ensure_occurrence_count_capacity_(std::size_t required_capacity) {
        using bioseq::common::saturating_add;
        using bioseq::common::saturating_mul;
        if (required_capacity > occurrence_count_capacity_) {
            const std::uint64_t retained = retained_capacity_bytes_();
            const std::uint64_t allocation =
                saturating_mul(required_capacity, sizeof(std::atomic<std::uint32_t>));
            const std::uint64_t peak = saturating_add(retained, allocation);
            if (allocation == (std::numeric_limits<std::uint64_t>::max)() ||
                peak == (std::numeric_limits<std::uint64_t>::max)() || !permit_.grow_to(peak)) {
                throw persisted_minimizer_lookup_error(
                    "fragment-native occurrence-count admission failed");
            }
            std::unique_ptr<std::atomic<std::uint32_t>[]> replacement;
            try {
                replacement = std::make_unique<std::atomic<std::uint32_t>[]>(required_capacity);
            } catch (const std::bad_alloc&) {
                permit_.shrink_to(retained_capacity_bytes_());
                throw persisted_minimizer_lookup_error(
                    "fragment-native occurrence-count allocation failed");
            }
            occurrence_counts_ = std::move(replacement);
            occurrence_count_capacity_ = required_capacity;
            const std::uint64_t actual = retained_capacity_bytes_();
            if (actual == (std::numeric_limits<std::uint64_t>::max)() || actual > permit_.bytes()) {
                release_all_capacity_noexcept_();
                throw persisted_minimizer_lookup_error(
                    "fragment-native occurrence-count reconciliation failed");
            }
            permit_.shrink_to(actual);
        }
        active_occurrence_count_ = required_capacity;
        for (std::size_t ordinal = 0u; ordinal < required_capacity; ++ordinal) {
            occurrence_counts_[ordinal].store(0u, std::memory_order_relaxed);
        }
    }

    void clear_wave_storage_noexcept_() noexcept {
        // The next activation zeros its exact U-sized range once. No reader
        // observes inactive counters, so clearing them here would add a second
        // O(U) atomic-store pass to every wave.
        active_occurrence_count_ = 0u;
        packed_occurrences_.clear();
        wide_occurrences_.clear();
        query_plans_.clear();
        query_group_offsets_.clear();
        query_plan_radix_.clear();
        block_query_layouts_.clear();
        block_stats_.clear();
        occurrence_layout_ = {};
        query_plan_reserve_request_ = 0u;
        occurrence_radix_invoked_ = false;
        parallel_occurrence_radix_split_selected_ = false;
        bucket_direct_matcher_active_ = false;
    }

    void reset_failed_wave_() noexcept {
        clear_wave_storage_noexcept_();
        wave_state_.store(wave_state::idle, std::memory_order_release);
    }

  public:
    query_wave_scratch(query_wave_scratch&& other) noexcept
        : permit_(std::move(other.permit_)),
          packed_occurrences_(std::move(other.packed_occurrences_)),
          wide_occurrences_(std::move(other.wide_occurrences_)),
          query_plans_(std::move(other.query_plans_)),
          query_group_offsets_(std::move(other.query_group_offsets_)),
          query_plan_radix_(std::move(other.query_plan_radix_)),
          query_membership_filter_(std::move(other.query_membership_filter_)),
          query_membership_filter_word_capacity_(other.query_membership_filter_word_capacity_),
          occurrence_counts_(std::move(other.occurrence_counts_)),
          occurrence_count_capacity_(other.occurrence_count_capacity_),
          active_occurrence_count_(other.active_occurrence_count_),
          block_query_layouts_(std::move(other.block_query_layouts_)),
          block_stats_(std::move(other.block_stats_)), occurrence_layout_(other.occurrence_layout_),
          query_plan_reserve_request_(other.query_plan_reserve_request_),
          occurrence_radix_invoked_(other.occurrence_radix_invoked_),
          parallel_occurrence_radix_split_selected_(
              other.parallel_occurrence_radix_split_selected_),
          bucket_direct_matcher_active_(other.bucket_direct_matcher_active_),
          admission_(other.admission_),
          wave_state_(other.wave_state_.exchange(wave_state::idle, std::memory_order_relaxed)) {
        other.occurrence_count_capacity_ = 0u;
        other.active_occurrence_count_ = 0u;
        other.occurrence_layout_ = {};
        other.query_plan_reserve_request_ = 0u;
        other.occurrence_radix_invoked_ = false;
        other.parallel_occurrence_radix_split_selected_ = false;
        other.query_membership_filter_word_capacity_ = 0u;
        other.bucket_direct_matcher_active_ = false;
        other.admission_ = nullptr;
    }

    query_wave_scratch& operator=(query_wave_scratch&&) = delete;
    query_wave_scratch(const query_wave_scratch&) = delete;
    query_wave_scratch& operator=(const query_wave_scratch&) = delete;

    [[nodiscard]] static std::expected<query_wave_scratch, minimizer_generation_encoding_error>
    create(bioseq::parallelism::memory_admission_controller& admission) {
        auto permit = admission.try_reserve(0u);
        if (!permit) {
            return std::unexpected(minimizer_generation_encoding_error::admission);
        }
        query_wave_scratch scratch;
        scratch.permit_ = std::move(*permit);
        scratch.admission_ = &admission;
        return scratch;
    }

    void complete_wave_after_join() noexcept {
        wave_state expected = wave_state::outstanding;
        if (wave_state_.compare_exchange_strong(expected,
                                                wave_state::cleaning,
                                                std::memory_order_acq_rel,
                                                std::memory_order_acquire)) {
            clear_wave_storage_noexcept_();
            wave_state_.store(wave_state::idle, std::memory_order_release);
        }
    }

    void release_retained_capacity() {
        wave_state expected = wave_state::idle;
        if (!wave_state_.compare_exchange_strong(expected,
                                                 wave_state::cleaning,
                                                 std::memory_order_acq_rel,
                                                 std::memory_order_acquire)) {
            throw persisted_minimizer_lookup_error("fragment-native query scratch is still in use");
        }
        release_all_capacity_noexcept_();
        wave_state_.store(wave_state::idle, std::memory_order_release);
    }

    [[nodiscard]] bool wave_outstanding() const noexcept {
        return wave_state_.load(std::memory_order_acquire) != wave_state::idle;
    }

    [[nodiscard]] std::uint64_t admitted_bytes() const noexcept {
        return permit_.bytes();
    }

    [[nodiscard]] query_occurrence_capacity_snapshot occurrence_capacity_snapshot() const noexcept {
        return query_occurrence_capacity_snapshot{
            .route = occurrence_layout_.route,
            .packed_size = packed_occurrences_.size(),
            .packed_capacity = packed_occurrences_.capacity(),
            .wide_size = wide_occurrences_.size(),
            .wide_capacity = wide_occurrences_.capacity(),
            .query_plan_size = query_plans_.size(),
            .query_plan_capacity = query_plans_.capacity(),
            .query_plan_reserve_request = query_plan_reserve_request_,
            .query_group_offset_size = query_group_offsets_.size(),
            .query_group_offset_capacity = query_group_offsets_.capacity(),
            .aggregate_occurrence_capacity_bytes = retained_occurrence_capacity_bytes_(),
            .query_membership_filter_capacity_bytes = bioseq::common::saturating_mul(
                query_membership_filter_word_capacity_, sizeof(std::uint64_t)),
            .occurrence_radix_invoked = occurrence_radix_invoked_,
            .parallel_occurrence_radix_split_selected = parallel_occurrence_radix_split_selected_,
            .bucket_direct_matcher_active = bucket_direct_matcher_active_,
        };
    }
};

/// Immutable, separately admitted final anchor owner.
class prepared_fragment_anchor_batch {
  private:
    static constexpr std::uint64_t sortedness_receipt_ = UINT64_C(0x46524147414E4348); // "FRAGANCH"

    // Declared first so exact anchors die before their permit.
    bioseq::parallelism::memory_permit permit_{};
    std::unique_ptr<prepared_query_anchors[]> queries_{};
    std::size_t query_count_ = 0u;
    std::uint64_t target_length_ = 0u;
    std::uint64_t max_occ_ = 0u;
    std::uint64_t owned_bytes_ = 0u;
    prepared_fragment_anchor_batch_stats stats_{};
    compact_direct_query_set_stats compact_direct_query_set_stats_{};
    bool ready_ = false;

    friend class prepared_minimizer_fragment_reader;

    explicit prepared_fragment_anchor_batch(bioseq::parallelism::memory_permit&& permit) noexcept
        : permit_(std::move(permit)) {}

  public:
    class query_handle {
      private:
        const prepared_fragment_anchor_batch* batch_ = nullptr;
        std::size_t query_index_ = 0u;

        friend class prepared_fragment_anchor_batch;

        query_handle(const prepared_fragment_anchor_batch& batch, std::size_t query_index) noexcept
            : batch_(&batch), query_index_(query_index) {}

      public:
        query_handle() = default;

        [[nodiscard]] std::uint64_t target_length() const noexcept {
            return batch_ == nullptr ? 0u : batch_->target_length_;
        }

        [[nodiscard]] std::span<const bioseq::chaining::anchor>
        ordered_fragment_seed_anchors(std::span<const minimap_sketch_data> minimizers,
                                      sequence_size_t query_length,
                                      std::int32_t max_occ,
                                      std::int32_t kmer_size) const {
            if (batch_ == nullptr || !batch_->ready_ || query_index_ >= batch_->query_count_ ||
                max_occ < 0 || static_cast<std::uint64_t>(max_occ) != batch_->max_occ_) {
                throw persisted_minimizer_lookup_error(
                    "prepared fragment anchor batch is inconsistent");
            }
            const prepared_query_anchors& query = batch_->queries_[query_index_];
            if (query.minimizer_data != minimizers.data() ||
                query.minimizer_count != minimizers.size() || query.query_length != query_length ||
                query.kmer_size != kmer_size || query.sortedness_receipt != sortedness_receipt_ ||
                query.anchor_count > (std::numeric_limits<std::size_t>::max)() ||
                (query.anchor_count != 0u && query.anchors == nullptr)) {
                throw persisted_minimizer_lookup_error(
                    "prepared fragment anchor query identity is inconsistent");
            }
            return std::span<const bioseq::chaining::anchor>{
                query.anchors.get(), static_cast<std::size_t>(query.anchor_count)};
        }
    };

    prepared_fragment_anchor_batch(prepared_fragment_anchor_batch&& other) noexcept
        : permit_(std::move(other.permit_)), queries_(std::move(other.queries_)),
          query_count_(other.query_count_), target_length_(other.target_length_),
          max_occ_(other.max_occ_), owned_bytes_(other.owned_bytes_), stats_(other.stats_),
          compact_direct_query_set_stats_(other.compact_direct_query_set_stats_),
          ready_(other.ready_) {
        other.query_count_ = 0u;
        other.target_length_ = 0u;
        other.max_occ_ = 0u;
        other.owned_bytes_ = 0u;
        other.stats_ = {};
        other.compact_direct_query_set_stats_ = {};
        other.ready_ = false;
    }

    prepared_fragment_anchor_batch& operator=(prepared_fragment_anchor_batch&&) = delete;
    prepared_fragment_anchor_batch(const prepared_fragment_anchor_batch&) = delete;
    prepared_fragment_anchor_batch& operator=(const prepared_fragment_anchor_batch&) = delete;

    [[nodiscard]] query_handle query_view(std::size_t query_index) const {
        if (!ready_ || query_index >= query_count_) {
            throw persisted_minimizer_lookup_error(
                "prepared fragment anchor query is out of range");
        }
        return query_handle{*this, query_index};
    }

    [[nodiscard]] std::size_t query_count() const noexcept {
        return query_count_;
    }

    [[nodiscard]] std::uint64_t admitted_bytes() const noexcept {
        return permit_.bytes();
    }

    [[nodiscard]] const prepared_fragment_anchor_batch_stats& stats() const noexcept {
        return stats_;
    }

    [[nodiscard]] const compact_direct_query_set_stats&
    compact_direct_query_stats() const noexcept {
        return compact_direct_query_set_stats_;
    }
};

} // namespace bioseq::indexing
