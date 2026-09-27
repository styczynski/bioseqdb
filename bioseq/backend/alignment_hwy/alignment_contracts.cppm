/**
 * @file alignment_contracts.cppm
 * @brief Alignment parameters, results and memory limits shared by SIMD kernels and schedulers.
 *
 * These types have no Highway dependency, so callers can use them without
 * selecting a CPU target.
 */
module;

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

export module bioseq.alignment.contracts;

export import bioseq.alignment.utils;
import bioseq.common.checked_arithmetic;
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
import bioseq.common.context;
import bioseq.common.heap_allocation;
import bioseq.parallelism.memory_admission;
#endif

export namespace bioseq::simd {

/** Integer type for score differences in generated KSW2 kernels. */
using ksw2_partial_score_type = std::int8_t;
/** Integer type for accumulated scores in generated KSW2 kernels. */
using ksw2_full_score_type = std::int32_t;
/** Integer type for SIMD scores in generated Block Aligner kernels. */
using block_aligner_score_type = std::int16_t;

#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
/**
 * Keep a memory reservation with each result-map node in E2E builds.
 *
 * The allocator stores the reservation before the aligned node payload and
 * adjusts it to the allocation's measured size. This lets nodes outlive the
 * code that created them without a separate table of reservations.
 *
 * std::map constructs the node. Until then, its pages cannot be counted as
 * fully touched, so the reservation stays with the allocation. Normal builds
 * use the usual map allocator.
 */
template <class p_value_type>
class admitted_alignment_node_allocator {
    template <class> friend class admitted_alignment_node_allocator;

    struct allocation_header {
        bioseq::parallelism::memory_permit permit{};
    };
    static_assert(alignof(allocation_header) <= alignof(std::max_align_t));

    [[nodiscard]] static std::size_t payload_bytes(const std::size_t count) {
        const auto bytes = bioseq::common::checked_mul(count, sizeof(p_value_type));
        if (!bytes) {
            throw std::bad_array_new_length{};
        }
        return *bytes;
    }

public:
    using value_type = p_value_type;
    using propagate_on_container_move_assignment = std::true_type;
    using propagate_on_container_swap = std::true_type;
    using is_always_equal = std::false_type;

    admitted_alignment_node_allocator() noexcept
        : admission_(std::addressof(
              bioseq::parallelism::memory_admission_controller::instance())) {}

    explicit admitted_alignment_node_allocator(
        bioseq::parallelism::memory_admission_controller& admission,
        bioseq::parallelism::memory_footprint_probe footprint_probe =
            bioseq::parallelism::live_process_footprint_probe) noexcept
        : admission_(std::addressof(admission)), footprint_probe_(footprint_probe) {}

    template <class p_other_type>
    admitted_alignment_node_allocator(
        const admitted_alignment_node_allocator<p_other_type>& other) noexcept
        : admission_(other.admission_), footprint_probe_(other.footprint_probe_) {}

    /** Calculate the memory needed for the payload, alignment and reservation header. */
    [[nodiscard]] static bioseq::common::heap_allocation_detail::heap_allocation_plan
    allocation_plan(const std::size_t count) {
        return bioseq::common::heap_allocation_detail::plan_aligned_heap_allocation(
            payload_bytes(count), alignof(p_value_type),
            sizeof(allocation_header) + sizeof(void*));
    }

    /** Reserve memory for a node and keep its reservation in the allocation header. */
    [[nodiscard]] p_value_type* allocate(const std::size_t count) {
        if (count == 0u) {
            return nullptr;
        }
        const auto plan = allocation_plan(count);
        const auto bytes = payload_bytes(count);
        auto permit = admission_->try_reserve(plan.allocation_requirement_bytes);
        if (!permit) {
            throw std::bad_alloc{};
        }
        void* base = nullptr;
        allocation_header* header = nullptr;
        void* payload = nullptr;
        std::size_t actual_bytes = 0u;
        try {
            (void)admission_->materialize(
                *permit,
                [&base, &header, &payload, &actual_bytes, &permit, plan, bytes]()
                    -> bioseq::parallelism::memory_materialization_snapshot {
                    base = bioseq::common::heap_allocation_detail::allocate_measured(
                        plan, actual_bytes);
                    payload = bioseq::common::heap_allocation_detail::aligned_heap_payload(
                        base, actual_bytes, bytes, alignof(p_value_type),
                        sizeof(allocation_header) + sizeof(void*));
                    if (payload == nullptr) {
                        throw std::bad_alloc{};
                    }
                    header = std::construct_at(static_cast<allocation_header*>(base));
                    std::memcpy(static_cast<std::byte*>(payload) - sizeof(void*),
                                std::addressof(base), sizeof(void*));
                    permit->shrink_to(actual_bytes);
                    if (permit->bytes() != actual_bytes) {
                        throw std::logic_error{"result node permit differs from its allocation"};
                    }
                    return {
                        .actual_allocation_bytes = actual_bytes,
                        // std::map still needs to construct the node. Allocating its
                        // header does not mean every payload page has been touched.
                        .fully_touched_bytes = 0u,
                        .is_host_visible = true,
                        .is_host_coherent = true,
                        .is_process_footprint_reflected = false,
                    };
                },
                footprint_probe_);
            // Keep the reservation with this allocation. The unconstructed node
            // may have pages missing from the process memory total.
            if (!*permit || permit->bytes() != actual_bytes) {
                throw std::logic_error{"unconstructed result node lost its exact permit"};
            }
            header->permit = std::move(*permit);
            return static_cast<p_value_type*>(payload);
        } catch (...) {
            if (header != nullptr) {
                std::destroy_at(header);
            }
            std::free(base);
            throw;
        }
    }

    /** Free the node allocation before releasing its memory reservation. */
    void deallocate(p_value_type* payload, std::size_t) noexcept {
        if (payload == nullptr) {
            return;
        }
        void* base = nullptr;
        std::memcpy(std::addressof(base),
                    reinterpret_cast<std::byte*>(payload) - sizeof(void*), sizeof(void*));
        auto* header = static_cast<allocation_header*>(base);
        auto permit = std::move(header->permit);
        std::destroy_at(header);
        std::free(base);
        permit.release();
    }

    template <class p_other_type>
    [[nodiscard]] bool operator==(
        const admitted_alignment_node_allocator<p_other_type>& other) const noexcept {
        return admission_ == other.admission_ && footprint_probe_ == other.footprint_probe_;
    }

private:
    bioseq::parallelism::memory_admission_controller* admission_;
    bioseq::parallelism::memory_footprint_probe footprint_probe_ =
        bioseq::parallelism::live_process_footprint_probe;
};

/**
 * @brief A vector with size limits and memory accounting.
 *
 * Growing keeps the old buffer accounted for until the replacement is ready,
 * then frees it before releasing its reservation. Fully touched pages may
 * instead be accounted for through the measured process memory total.
 *
 * The selected specialization uses a worker allocation hook or an explicit
 * controller. E2E alignment results use this vector. Ordinary pipeline builds
 * keep std::vector, while tests can use the checked implementation directly.
 */
template <class p_value_type, bool p_context_backed = true>
class admitted_alignment_result_vector {
    static constexpr bool k_can_touch_complete_capacity =
        std::is_trivially_copyable_v<p_value_type> &&
        std::is_nothrow_default_constructible_v<p_value_type> &&
        std::is_nothrow_copy_constructible_v<p_value_type>;
    static constexpr bool k_can_transfer_after_admission =
        std::is_nothrow_move_constructible_v<p_value_type> &&
        std::is_nothrow_destructible_v<p_value_type>;
    static_assert(k_can_touch_complete_capacity || k_can_transfer_after_admission);

    struct materialization_state {
        const std::vector<p_value_type>* source = nullptr;
        std::vector<p_value_type> replacement{};
        std::size_t requested_capacity = 0u;
        std::uint64_t admitted_bound_bytes = 0u;
    };

    [[nodiscard]] static std::uint64_t capacity_bytes(std::size_t capacity) {
        const auto bytes = bioseq::common::checked_mul(
            static_cast<std::uint64_t>(capacity), sizeof(p_value_type));
        if (!bytes || *bytes > (std::numeric_limits<std::size_t>::max)()) {
            throw std::length_error{"alignment vector capacity is not addressable"};
        }
        return *bytes;
    }

    [[nodiscard]] static context::backing_allocation_materialization
    materialize_replacement(
        void* opaque_state,
        const context::backing_allocation_reconciliation& reconciliation) {
        auto& state = *static_cast<materialization_state*>(opaque_state);
        state.replacement.reserve(state.requested_capacity);
        const std::uint64_t actual_bytes = capacity_bytes(state.replacement.capacity());
        if (actual_bytes > state.admitted_bound_bytes ||
            !reconciliation.reconcile_to(actual_bytes)) {
            throw std::bad_alloc{};
        }
        if constexpr (k_can_touch_complete_capacity) {
            state.replacement.insert(
                state.replacement.end(), state.source->begin(), state.source->end());
            const std::size_t retained_size = state.replacement.size();
            state.replacement.resize(state.replacement.capacity());
            if (state.replacement.size() > retained_size) {
                std::memset(
                    state.replacement.data() + retained_size,
                    0,
                    (state.replacement.size() - retained_size) * sizeof(p_value_type));
            }
            state.replacement.resize(retained_size);
        }
        return {
            .actual_allocation_bytes = actual_bytes,
            .fully_touched_bytes = k_can_touch_complete_capacity ? actual_bytes : 0u,
            .primary_owner_bytes = actual_bytes,
            .replacement_owner_bytes = 0u,
            .is_host_visible = true,
            .is_host_coherent = true,
            .is_process_footprint_reflected = k_can_touch_complete_capacity,
        };
    }

    static void cleanup_replacement(void* opaque_state) noexcept {
        auto& state = *static_cast<materialization_state*>(opaque_state);
        std::vector<p_value_type> empty;
        state.replacement.swap(empty);
    }

    [[nodiscard]] static bool reconcile_direct_permit(
        void* opaque_permit, const std::uint64_t actual_bytes) noexcept {
        auto& permit =
            *static_cast<bioseq::parallelism::memory_permit*>(opaque_permit);
        if (!permit.grow_to(actual_bytes)) {
            return false;
        }
        permit.shrink_to(actual_bytes);
        return permit.bytes() == actual_bytes;
    }

    [[nodiscard]] context::backing_allocation_authority materialize_direct_replacement(
        const std::uint64_t admitted_bound,
        materialization_state& state) {
        if (direct_admission_ == nullptr) {
            throw std::logic_error{"direct alignment vector has no admission controller"};
        }
        auto permit = direct_admission_->try_reserve(admitted_bound);
        if (!permit) {
            throw std::bad_alloc{};
        }
        try {
            context::backing_allocation_materialization allocation{};
            const auto materialization = direct_admission_->materialize(
                *permit,
                [&allocation, &permit, &state]()
                    -> bioseq::parallelism::memory_materialization_snapshot {
                    const context::backing_allocation_reconciliation reconciliation{
                        .state = std::addressof(*permit),
                        .reconcile = &admitted_alignment_result_vector::reconcile_direct_permit,
                    };
                    allocation = materialize_replacement(
                        std::addressof(state), reconciliation);
                    if (allocation.actual_allocation_bytes != permit->bytes()) {
                        throw std::logic_error{
                            "direct alignment vector did not reconcile exact capacity"};
                    }
                    return {
                        .actual_allocation_bytes = allocation.actual_allocation_bytes,
                        .fully_touched_bytes = allocation.fully_touched_bytes,
                        .is_host_visible = allocation.is_host_visible,
                        .is_host_coherent = allocation.is_host_coherent,
                        .is_process_footprint_reflected =
                            allocation.is_process_footprint_reflected,
                    };
                },
                direct_footprint_probe_);
            if (allocation.primary_owner_bytes != allocation.actual_allocation_bytes ||
                allocation.replacement_owner_bytes != 0u ||
                materialization.admitted_bytes != allocation.actual_allocation_bytes ||
                materialization.is_permit_released == static_cast<bool>(*permit)) {
                throw std::logic_error{
                    "direct alignment vector materialization proof is inconsistent"};
            }
            context::backing_allocation_authority authority{};
            if (*permit) {
                authority.primary.emplace(std::move(*permit));
            }
            return authority;
        } catch (...) {
            cleanup_replacement(std::addressof(state));
            throw;
        }
    }

    [[nodiscard]] std::size_t geometric_capacity(std::size_t minimum) const {
        if (minimum > logical_size_bound_ || minimum > values_.max_size()) {
            throw std::length_error{"alignment vector size exceeds max_size"};
        }
        if (values_.capacity() == 0u) {
            return minimum;
        }
        if (values_.capacity() > values_.max_size() / 2u) {
            return minimum;
        }
        return std::min(
            logical_capacity_bound_,
            std::max(minimum, values_.capacity() * 2u));
    }

public:
    using value_type = p_value_type;
    using size_type = typename std::vector<p_value_type>::size_type;
    using iterator = typename std::vector<p_value_type>::iterator;
    using const_iterator = typename std::vector<p_value_type>::const_iterator;

    admitted_alignment_result_vector() noexcept {
        if constexpr (!p_context_backed) {
            direct_admission_ = std::addressof(
                bioseq::parallelism::memory_admission_controller::instance());
            direct_footprint_probe_ = bioseq::parallelism::live_process_footprint_probe;
        }
    }

    explicit admitted_alignment_result_vector(
        bioseq::parallelism::memory_admission_controller& admission,
        const bioseq::parallelism::memory_footprint_probe footprint_probe =
            bioseq::parallelism::live_process_footprint_probe) noexcept
        requires (!p_context_backed)
        : direct_admission_(std::addressof(admission)),
          direct_footprint_probe_(footprint_probe) {}
    admitted_alignment_result_vector(const admitted_alignment_result_vector&) = delete;
    admitted_alignment_result_vector& operator=(const admitted_alignment_result_vector&) = delete;

    admitted_alignment_result_vector(admitted_alignment_result_vector&& other) noexcept
        : capacity_lease_(std::move(other.capacity_lease_)),
          values_(std::move(other.values_)),
          logical_size_bound_(other.logical_size_bound_),
          logical_capacity_bound_(other.logical_capacity_bound_),
          direct_admission_(other.direct_admission_),
          direct_footprint_probe_(other.direct_footprint_probe_) {}

    admitted_alignment_result_vector& operator=(admitted_alignment_result_vector&& other) noexcept {
        if (this != &other) {
            {
                std::vector<p_value_type> empty;
                values_.swap(empty);
            }
            capacity_lease_.reset();
            capacity_lease_ = std::move(other.capacity_lease_);
            values_ = std::move(other.values_);
            logical_size_bound_ = other.logical_size_bound_;
            logical_capacity_bound_ = other.logical_capacity_bound_;
            direct_admission_ = other.direct_admission_;
            direct_footprint_probe_ = other.direct_footprint_probe_;
        }
        return *this;
    }

    /** Grow the vector while accounting for both buffers until the old one is freed. */
    void reserve(std::size_t requested_capacity) {
        if (requested_capacity > logical_capacity_bound_) {
            throw std::length_error{"alignment vector exceeded its logical capacity bound"};
        }
        if (requested_capacity <= values_.capacity()) {
            return;
        }
        const std::uint64_t logical_bytes = capacity_bytes(requested_capacity);
        const auto admitted_bound = bioseq::common::checked_mul(
            logical_bytes,
            bioseq::parallelism::k_vector_preallocation_headroom_factor);
        if (!admitted_bound) {
            throw std::length_error{"alignment vector replacement bound overflowed"};
        }
        materialization_state state{
            .source = std::addressof(values_),
            .requested_capacity = requested_capacity,
            .admitted_bound_bytes = *admitted_bound,
        };
        context::backing_allocation_authority authority{};
        if constexpr (p_context_backed) {
            auto& installed_context = context::current();
            if (!installed_context.has_backing_allocation_hook()) {
                throw std::logic_error{
                    "campaign alignment vector has no installed admission hook"};
            }
            authority = installed_context.materialize_adjacent_allocation(
                *admitted_bound,
                std::addressof(state),
                &admitted_alignment_result_vector::materialize_replacement,
                &admitted_alignment_result_vector::cleanup_replacement);
        } else {
            authority = materialize_direct_replacement(*admitted_bound, state);
        }
        if (authority.replacement) {
            cleanup_replacement(std::addressof(state));
            throw std::logic_error{
                "alignment vector replacement returned an unexpected second lease"};
        }
        if constexpr (!k_can_touch_complete_capacity) {
            if (!authority.primary) {
                cleanup_replacement(std::addressof(state));
                throw std::logic_error{
                    "nontrivial alignment vector replacement lost its exact lease"};
            }
            for (p_value_type& value : values_) {
                state.replacement.emplace_back(std::move(value));
            }
        }
        {
            std::vector<p_value_type> old_values;
            values_.swap(old_values);
            values_.swap(state.replacement);
        }
        capacity_lease_.reset();
        capacity_lease_ = std::move(authority.primary);
    }

    void resize(std::size_t count) {
        if (count > logical_size_bound_) {
            throw std::length_error{"alignment vector exceeded its logical size bound"};
        }
        if (count > values_.capacity()) {
            reserve(count);
        }
        values_.resize(count);
    }

    void clear() noexcept { values_.clear(); }

    /** Set the maximum size and requested capacity before growing the vector. */
    void set_logical_bounds(std::size_t size_bound, std::size_t capacity_bound) {
        if (values_.size() > size_bound || size_bound > capacity_bound ||
            capacity_bound > values_.max_size()) {
            throw std::length_error{"alignment vector logical bounds are not representable"};
        }
        logical_size_bound_ = size_bound;
        logical_capacity_bound_ = capacity_bound;
    }

    void push_back(const p_value_type& value)
        requires std::is_copy_constructible_v<p_value_type>
    {
        if (values_.size() >= logical_size_bound_) {
            throw std::length_error{"alignment vector exceeded its logical element bound"};
        }
        if (values_.size() == values_.capacity()) {
            reserve(geometric_capacity(values_.size() + 1u));
        }
        values_.push_back(value);
    }

    void push_back(p_value_type&& value) {
        if (values_.size() >= logical_size_bound_) {
            throw std::length_error{"alignment vector exceeded its logical element bound"};
        }
        if (values_.size() == values_.capacity()) {
            reserve(geometric_capacity(values_.size() + 1u));
        }
        values_.push_back(std::move(value));
    }

    template <class... p_arguments>
    p_value_type& emplace_back(p_arguments&&... arguments) {
        if (values_.size() >= logical_size_bound_) {
            throw std::length_error{"alignment vector exceeded its logical element bound"};
        }
        if (values_.size() == values_.capacity()) {
            reserve(geometric_capacity(values_.size() + 1u));
        }
        return values_.emplace_back(std::forward<p_arguments>(arguments)...);
    }

    [[nodiscard]] bool empty() const noexcept { return values_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return values_.size(); }
    [[nodiscard]] std::size_t capacity() const noexcept { return values_.capacity(); }
    [[nodiscard]] std::size_t max_size() const noexcept { return values_.max_size(); }
    [[nodiscard]] p_value_type* data() noexcept { return values_.data(); }
    [[nodiscard]] const p_value_type* data() const noexcept { return values_.data(); }
    [[nodiscard]] iterator begin() noexcept { return values_.begin(); }
    [[nodiscard]] const_iterator begin() const noexcept { return values_.begin(); }
    [[nodiscard]] const_iterator cbegin() const noexcept { return values_.cbegin(); }
    [[nodiscard]] iterator end() noexcept { return values_.end(); }
    [[nodiscard]] const_iterator end() const noexcept { return values_.end(); }
    [[nodiscard]] const_iterator cend() const noexcept { return values_.cend(); }
    [[nodiscard]] p_value_type& front() noexcept { return values_.front(); }
    [[nodiscard]] const p_value_type& front() const noexcept { return values_.front(); }
    [[nodiscard]] p_value_type& back() noexcept { return values_.back(); }
    [[nodiscard]] const p_value_type& back() const noexcept { return values_.back(); }
    [[nodiscard]] p_value_type& operator[](std::size_t index) noexcept { return values_[index]; }
    [[nodiscard]] const p_value_type& operator[](std::size_t index) const noexcept {
        return values_[index];
    }

    [[nodiscard]] friend bool operator==(
        const admitted_alignment_result_vector& left,
        const admitted_alignment_result_vector& right) noexcept
        requires requires(const std::vector<p_value_type>& left_values,
                          const std::vector<p_value_type>& right_values) {
            left_values == right_values;
        }
    {
        return left.values_ == right.values_;
    }

private:
    // Members are destroyed in reverse order. Free the vector before
    // releasing its remaining memory reservation.
    context::backing_allocation_lease capacity_lease_{};
    std::vector<p_value_type> values_{};
    std::size_t logical_size_bound_ = 0u;
    std::size_t logical_capacity_bound_ = 0u;
    bioseq::parallelism::memory_admission_controller* direct_admission_ = nullptr;
    bioseq::parallelism::memory_footprint_probe direct_footprint_probe_ = nullptr;
};

#endif

#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
template <class p_value_type>
using alignment_result_vector = admitted_alignment_result_vector<p_value_type>;
#else
template <class p_value_type>
using alignment_result_vector = std::vector<p_value_type>;
#endif

#if defined(BIOSEQ_TEST_INDEXES)
/** Expose the E2E vector to tests without changing ordinary pipeline result types. */
template <class p_value_type>
using test_alignment_result_vector = admitted_alignment_result_vector<p_value_type>;
/** A vector that reserves memory through an explicit controller outside a worker context. */
template <class p_value_type>
using test_outer_alignment_result_vector =
    admitted_alignment_result_vector<p_value_type, false>;
#endif

namespace detail_ksw2_batched_ordering {

/** Group KSW2 fills with similar lengths, bandwidth and query orientation. */
struct fill_bucket_key {
    std::int32_t maximum_query_length;  ///< Query-length bucket, or exact length for a long fill.
    std::int32_t maximum_target_length;  ///< Target-length bucket, or exact length for a long fill.
    std::int32_t maximum_bandwidth;  ///< Bandwidth bucket, or exact bandwidth for a long fill.
    bool is_long_fill;  ///< Whether either sequence is too long for the short-fill buckets.
    bool is_reversed;  ///< Whether query ranks use reverse orientation.

    [[nodiscard]] friend constexpr bool operator==(
        const fill_bucket_key&, const fill_bucket_key&) noexcept = default;
};

/** Length limits for grouping short KSW2 fills. */
inline constexpr std::array<std::int32_t, 4> k_fill_length_brackets = {
    32, 64, 128, 256};
/** Bandwidth limits for grouping short KSW2 fills. */
inline constexpr std::array<std::int32_t, 3> k_fill_band_brackets = {
    16, 32, 64};

/** Round up to the first bucket that fits, keeping values above the last bucket unchanged. */
[[nodiscard, gnu::always_inline]] inline auto
round_up_to_bracket(std::int32_t value, std::span<const std::int32_t> brackets) noexcept
    -> std::int32_t {
    for (const std::int32_t upper_bound : brackets) {
        if (value <= upper_bound) {
            return upper_bound;
        }
    }
    return value;
}

/** Group short fills by size and keep exact dimensions for long fills. */
template <class p_fill_descriptor>
[[nodiscard, gnu::always_inline]] inline auto
bucket_key_for(const p_fill_descriptor& fill, std::int32_t default_bandwidth) noexcept
    -> fill_bucket_key {
    const bool is_long = (fill.query_length > 256) || (fill.target_length > 256);
    if (is_long) {
        return fill_bucket_key{
            .maximum_query_length = fill.query_length,
            .maximum_target_length = fill.target_length,
            .maximum_bandwidth = fill.bandwidth > 0 ? fill.bandwidth : default_bandwidth,
            .is_long_fill = true,
            .is_reversed = fill.is_reversed,
        };
    }
    return fill_bucket_key{
        .maximum_query_length = round_up_to_bracket(
            fill.query_length, std::span<const std::int32_t>{k_fill_length_brackets}),
        .maximum_target_length = round_up_to_bracket(
            fill.target_length, std::span<const std::int32_t>{k_fill_length_brackets}),
        .maximum_bandwidth =
            round_up_to_bracket(fill.bandwidth > 0 ? fill.bandwidth : default_bandwidth,
                                std::span<const std::int32_t>{k_fill_band_brackets}),
        .is_long_fill = false,
        .is_reversed = fill.is_reversed,
    };
}

/** Order fills by size and orientation, then by their original position in the region. */
struct by_bucket_then_order {
    std::int32_t default_bandwidth;  ///< Bandwidth to use when a fill has no positive bandwidth.

    template <class p_fill_descriptor>
    [[nodiscard, gnu::always_inline]] inline auto
    operator()(const p_fill_descriptor& left, const p_fill_descriptor& right) const noexcept
        -> bool {
        const fill_bucket_key left_key = bucket_key_for(left, default_bandwidth);
        const fill_bucket_key right_key = bucket_key_for(right, default_bandwidth);
        if (left_key.is_long_fill != right_key.is_long_fill) {
            return !left_key.is_long_fill;
        }
        if (left_key.is_reversed != right_key.is_reversed) {
            return !left_key.is_reversed;
        }
        if (left_key.maximum_query_length
            != right_key.maximum_query_length) {
            return left_key.maximum_query_length
                < right_key.maximum_query_length;
        }
        if (left_key.maximum_target_length
            != right_key.maximum_target_length) {
            return left_key.maximum_target_length
                < right_key.maximum_target_length;
        }
        if (left_key.maximum_bandwidth != right_key.maximum_bandwidth) {
            return left_key.maximum_bandwidth
                < right_key.maximum_bandwidth;
        }
        if (left.region_index != right.region_index) {
            return left.region_index < right.region_index;
        }
        return left.fill_order < right.fill_order;
    }
};

/** Compare fills by their original region and fill order. */
struct chain_order {
    template <class p_fill_descriptor>
    [[nodiscard, gnu::always_inline]] inline auto
    operator()(const p_fill_descriptor& left, const p_fill_descriptor& right) const noexcept
        -> bool {
        if (left.region_index != right.region_index) {
            return left.region_index < right.region_index;
        }
        return left.fill_order < right.fill_order;
    }
};

}  // namespace detail_ksw2_batched_ordering

/** KSW2 options shared by the single- and dual-affine gap models. */
struct ksw_flags {
    bool score_only    : 1 = false;  ///< Suppress traceback generation.
    bool right_align   : 1 = false;  ///< Prefer rightmost equivalent traceback.
    bool generic_score : 1 = false;  ///< Look up each substitution score in the full scoring matrix.
    bool approx_max    : 1 = false;  ///< Follow an approximate best path instead of reconstructing every full score.
    bool approx_zdrop  : 1 = false;  ///< Apply Z-drop to the approximate path when approx_max is enabled.
    bool extz_only     : 1 = false;  ///< Trace back an extension endpoint, using end_bonus when the query end is reached.
    bool reverse_cigar : 1 = false;  ///< Read intervals backward and keep CIGAR operations in reverse order.
    bool reserved      : 1 = false;  ///< Reserved layout bit. Keep false.
};
static_assert(sizeof(ksw_flags) == 1);

/** KSW2 parameters with one affine gap cost: gap_open + length * gap_extend. */
struct ksw_single_affine_parameters {
    int8_t    gap_open   = 0;   ///< Positive gap-open penalty.
    int8_t    gap_extend = 0;   ///< Positive gap-extension penalty.
    int       band_width = -1;  ///< Band width, or negative for unrestricted.
    int       z_drop     = -1;  ///< Z-drop threshold, or negative to disable.
    int       end_bonus  = 0;   ///< Bonus used when choosing traceback through the query end.
    ksw_flags flags      = {};  ///< Scoring, stopping and traceback options.
};

/** KSW2 parameters that use the cheaper of the short-gap and long-gap affine costs. */
struct ksw_dual_affine_parameters {
    int8_t gap_open_short   = 0;   ///< Positive short-gap open penalty.
    int8_t gap_extend_short = 0;   ///< Positive short-gap extension penalty.
    int8_t gap_open_long    = 0;   ///< Positive long-gap open penalty.
    int8_t gap_extend_long  = 0;   ///< Positive long-gap extension penalty.
    int    band_width       = -1;  ///< Band width, or negative for unrestricted.
    int    z_drop           = -1;  ///< Z-drop threshold, or negative to disable.
    int    end_bonus        = 0;   ///< Bonus used when choosing traceback through the query end.
    ksw_flags flags         = {};  ///< Scoring, stopping and traceback options.
};

/** Score used before an alignment endpoint has been reached. */
constexpr int32_t k_ksw_negative_infinity = -0x40000000;

/**
 * Result shared by KSW2 and Block Aligner.
 * Coordinates are zero-based positions within the supplied intervals. A value
 * of -1 means no base was consumed. CIGAR operations use KSW2's packed format.
 */
struct ksw_alignment_result {
    int32_t max       = 0;  ///< Best extension score. Block global alignment stores its final score here.
    int32_t score     = k_ksw_negative_infinity;  ///< Final score, or negative infinity if KSW2 did not reach the final cell.
    int32_t mqe       = k_ksw_negative_infinity;  ///< Best query-end score.
    int32_t mte       = k_ksw_negative_infinity;  ///< Best target-end score.
    int32_t max_q     = -1;  ///< Query coordinate of the maximum score.
    int32_t max_t     = -1;  ///< Target coordinate of the maximum score.
    int32_t mqe_t     = -1;  ///< Target coordinate for the query-end maximum.
    int32_t mte_q     = -1;  ///< Query coordinate for the target-end maximum.
    bool is_z_dropped = false;  ///< Whether KSW2 stopped at Z-drop or Block Aligner stopped at X-drop.
    bool has_reached_end = false;  ///< Whether extension selected the query end. Also set for Block global alignment.
    std::uint64_t edit_distance = 0;  ///< Mismatches and gap bases in the selected alignment path.
    bool is_edit_distance_valid = false;  ///< Whether edit_distance is populated.
    alignment_result_vector<uint32_t> cigar;  ///< Packed KSW2 CIGAR operations.
};
/** The part of a chained alignment handled by this Block Aligner call. */
enum class block_alignment_stage : std::uint8_t {
    left_extension,
    gap_extension,
    right_extension,
};

/// Fill an entire gap or extend a flank with X-drop.
/// X-drop stops after the block maximum stays below the best score by the
/// threshold for two consecutive steps.
enum class block_alignment_mode : std::uint8_t {
    global,
    x_drop,
};

/** Order in which Block Aligner reads the selected intervals. */
enum class block_input_order : std::uint8_t {
    forward,
    reversed,
};

/**
 * Count requested and completed Block Aligner work.
 * Counters saturate on overflow. Configuration checksums intentionally wrap
 * so requested and executed calls can be compared after combining records.
 */
struct block_aligner_execution_stats {
    std::uint64_t align_region_invocations = 0;  ///< Region entry-point calls.
    std::uint64_t serial_align_region_dispatches = 0;  ///< Serial region calls.
    std::uint64_t parallel_align_region_dispatches = 0;  ///< Parallel region calls.
    std::uint64_t routed_region_scopes = 0;  ///< Region scopes routed to alignment.
    std::uint64_t left_route_candidates = 0;  ///< Routed left extensions.
    std::uint64_t gap_route_candidates = 0;  ///< Routed inter-seed gaps.
    std::uint64_t right_route_candidates = 0;  ///< Routed right extensions.
    std::uint64_t left_eligible = 0;  ///< Validated left-extension requests.
    std::uint64_t left_executed = 0;  ///< Executed left extensions.
    std::uint64_t gap_eligible = 0;  ///< Validated inter-seed-gap requests.
    std::uint64_t gap_executed = 0;  ///< Executed inter-seed gaps.
    std::uint64_t right_eligible = 0;  ///< Validated right-extension requests.
    std::uint64_t right_executed = 0;  ///< Executed right extensions.
    std::uint64_t global_calls = 0;  ///< Requested global alignments.
    std::uint64_t native_x_drop_calls = 0;  ///< Requested X-drop extensions.
    std::uint64_t native_x_drop_terminations = 0;  ///< Calls stopped by X-drop.
    std::uint64_t zero_work_calls = 0;  ///< Calls with an empty sequence extent.
    std::uint64_t nonempty_calls = 0;  ///< Calls with both sequence intervals nonempty.
    std::uint64_t nonempty_calls_with_simd = 0;  ///< Nonempty calls using SIMD.
    std::uint64_t trace_calls = 0;  ///< Requests with traceback enabled.
    std::uint64_t executed_trace_calls = 0;  ///< Executed traceback requests.
    std::uint64_t forward_input_calls = 0;  ///< Requests using forward inputs.
    std::uint64_t reversed_input_calls = 0;  ///< Requests using reversed inputs.
    std::uint64_t executed_global_calls = 0;  ///< Executed global alignments.
    std::uint64_t executed_native_x_drop_calls = 0;  ///< Executed X-drop alignments.
    std::uint64_t executed_forward_input_calls = 0;  ///< Executed forward inputs.
    std::uint64_t executed_reversed_input_calls = 0;  ///< Executed reversed inputs.
    std::uint64_t simd_eight_lane_calls = 0;  ///< Calls using eight score lanes.
    std::uint64_t simd_sixteen_lane_calls = 0;  ///< Calls using sixteen score lanes.
    std::uint64_t simd_dp_vector_batches = 0;  ///< DP vector batches evaluated.
    std::uint64_t simd_dp_cells = 0;  ///< DP cells evaluated.
    // A SIMD scan can prove that an equal-length gap has one best path:
    // its diagonal. Count those scans separately so simd_dp_cells reports
    // only the cells evaluated by align_block_core.
    std::uint64_t simd_diagonal_certificate_calls = 0;  ///< Gaps whose diagonal was proved to be the unique best path.
    std::uint64_t simd_diagonal_certificate_vector_batches = 0;  ///< SIMD batches used by successful diagonal checks.
    std::uint64_t simd_diagonal_certificate_symbols = 0;  ///< Symbols compared by successful diagonal checks.
    std::uint64_t adaptive_grow_events = 0;  ///< Adaptive block growth events.
    std::uint64_t adaptive_shrink_events = 0;  ///< Adaptive block shrink events.
    std::uint64_t expected_reversed_compact_symbols = 0;  ///< Symbols that should be emitted in reverse order.
    std::uint64_t reversed_compact_symbols = 0;  ///< Symbols emitted in reverse order from compact storage.
    std::uint64_t configuration_identity_calls = 0;  ///< Calls included in the requested-configuration checksums.
    std::uint64_t configuration_identity_xor = 0;  ///< XOR of requested configuration hashes.
    std::uint64_t configuration_identity_sum = 0;  ///< Sum of requested configuration hashes, with unsigned wraparound.
    std::uint64_t executed_configuration_identity_calls = 0;  ///< Calls included in the executed-configuration checksums.
    std::uint64_t executed_configuration_identity_xor = 0;  ///< XOR of executed configuration hashes.
    std::uint64_t executed_configuration_identity_sum = 0;  ///< Sum of executed configuration hashes, with unsigned wraparound.
    std::uint64_t unsupported_contract_rejections = 0;  ///< Calls rejected because their parameters or inputs are unsupported.
    std::uint64_t forbidden_ksw2_calls = 0;  ///< Forbidden KSW2 fallbacks.
    std::uint64_t forbidden_scalar_biological_fallbacks = 0;  ///< Forbidden attempts to use scalar alignment as a fallback.
    bool is_saturated = false;  ///< Whether any counter saturated.

    /** Return whether an unsigned counter increment would overflow. */
    [[nodiscard]] static constexpr auto add_would_overflow(std::uint64_t destination,
                                                           std::uint64_t increment) noexcept
        -> bool {
        return increment > (std::numeric_limits<std::uint64_t>::max)()
            - destination;
    }

    /** Add to a counter, saturating and recording overflow. */
    static auto add(std::uint64_t& destination, std::uint64_t increment, bool& saturation) noexcept
        -> void {
        if (add_would_overflow(destination, increment)) {
            destination = (std::numeric_limits<std::uint64_t>::max)();
            saturation = true;
            return;
        }
        destination += increment;
    }

    /** Add to a member counter using this record's saturation flag. */
    auto add(std::uint64_t& destination, std::uint64_t increment = 1) noexcept -> void {
        add(destination, increment, is_saturated);
    }

    /** Combine counters and checksums from another execution record. */
    auto merge(const block_aligner_execution_stats& other) noexcept -> void {
        is_saturated = is_saturated || other.is_saturated;
        add(align_region_invocations, other.align_region_invocations);
        add(serial_align_region_dispatches,
            other.serial_align_region_dispatches);
        add(parallel_align_region_dispatches,
            other.parallel_align_region_dispatches);
        add(routed_region_scopes, other.routed_region_scopes);
        add(left_route_candidates, other.left_route_candidates);
        add(gap_route_candidates, other.gap_route_candidates);
        add(right_route_candidates, other.right_route_candidates);
        add(left_eligible, other.left_eligible);
        add(left_executed, other.left_executed);
        add(gap_eligible, other.gap_eligible);
        add(gap_executed, other.gap_executed);
        add(right_eligible, other.right_eligible);
        add(right_executed, other.right_executed);
        add(global_calls, other.global_calls);
        add(native_x_drop_calls, other.native_x_drop_calls);
        add(native_x_drop_terminations, other.native_x_drop_terminations);
        add(zero_work_calls, other.zero_work_calls);
        add(nonempty_calls, other.nonempty_calls);
        add(nonempty_calls_with_simd, other.nonempty_calls_with_simd);
        add(trace_calls, other.trace_calls);
        add(executed_trace_calls, other.executed_trace_calls);
        add(forward_input_calls, other.forward_input_calls);
        add(reversed_input_calls, other.reversed_input_calls);
        add(executed_global_calls, other.executed_global_calls);
        add(executed_native_x_drop_calls, other.executed_native_x_drop_calls);
        add(executed_forward_input_calls, other.executed_forward_input_calls);
        add(executed_reversed_input_calls, other.executed_reversed_input_calls);
        add(simd_eight_lane_calls, other.simd_eight_lane_calls);
        add(simd_sixteen_lane_calls, other.simd_sixteen_lane_calls);
        add(simd_dp_vector_batches, other.simd_dp_vector_batches);
        add(simd_dp_cells, other.simd_dp_cells);
        add(simd_diagonal_certificate_calls,
            other.simd_diagonal_certificate_calls);
        add(simd_diagonal_certificate_vector_batches,
            other.simd_diagonal_certificate_vector_batches);
        add(simd_diagonal_certificate_symbols,
            other.simd_diagonal_certificate_symbols);
        add(adaptive_grow_events, other.adaptive_grow_events);
        add(adaptive_shrink_events, other.adaptive_shrink_events);
        add(expected_reversed_compact_symbols,
            other.expected_reversed_compact_symbols);
        add(reversed_compact_symbols, other.reversed_compact_symbols);
        add(configuration_identity_calls, other.configuration_identity_calls);
        configuration_identity_xor ^= other.configuration_identity_xor;
        // These sums wrap at 2^64 as part of the checksum. Pair them with
        // XOR to compare requested and executed configurations.
        configuration_identity_sum += other.configuration_identity_sum;
        add(executed_configuration_identity_calls,
            other.executed_configuration_identity_calls);
        executed_configuration_identity_xor ^= other.executed_configuration_identity_xor;
        executed_configuration_identity_sum += other.executed_configuration_identity_sum;
        add(unsupported_contract_rejections,
            other.unsupported_contract_rejections);
        add(forbidden_ksw2_calls, other.forbidden_ksw2_calls);
        add(forbidden_scalar_biological_fallbacks,
            other.forbidden_scalar_biological_fallbacks);
    }

    /** Sum three counters, returning false if the total would overflow. */
    [[nodiscard]] static constexpr auto sum_three_counters(std::uint64_t first,
                                                           std::uint64_t second,
                                                           std::uint64_t third,
                                                           std::uint64_t& result) noexcept -> bool {
        if (add_would_overflow(first, second)) return false;
        const std::uint64_t partial = first + second;
        if (add_would_overflow(partial, third)) return false;
        result = partial + third;
        return true;
    }

    /** Return the number of eligible calls, or UINT64_MAX on overflow. */
    [[nodiscard]] constexpr auto eligible_calls() const noexcept -> std::uint64_t {
        std::uint64_t result = (std::numeric_limits<std::uint64_t>::max)();
        (void)sum_three_counters(left_eligible, gap_eligible, right_eligible, result);
        return result;
    }

    /** Return the number of executed calls, or UINT64_MAX on overflow. */
    [[nodiscard]] constexpr auto executed_calls() const noexcept -> std::uint64_t {
        std::uint64_t result = (std::numeric_limits<std::uint64_t>::max)();
        (void)sum_three_counters(left_executed, gap_executed, right_executed, result);
        return result;
    }

    /**
     * Check that every requested call has matching execution and SIMD work.
     * Also check stage, input order and configuration counts, and reject any
     * overflow, unsupported call or forbidden fallback.
     */
    [[nodiscard]] constexpr auto is_internally_consistent() const noexcept -> bool {
        std::uint64_t eligible_total = 0;
        std::uint64_t executed_total = 0;
        std::uint64_t requested_xdrop_total = 0;
        std::uint64_t executed_xdrop_total = 0;
        std::uint64_t requested_forward_total = 0;
        std::uint64_t executed_forward_total = 0;
        std::uint64_t work_total = 0;
        std::uint64_t lane_total = 0;
        const bool totals_valid =
            sum_three_counters(left_eligible, gap_eligible, right_eligible, eligible_total)
            && sum_three_counters(left_executed, gap_executed, right_executed, executed_total)
            && !add_would_overflow(left_eligible, right_eligible)
            && !add_would_overflow(left_executed, right_executed)
            && !add_would_overflow(gap_eligible, right_eligible)
            && !add_would_overflow(gap_executed, right_executed)
            && !add_would_overflow(zero_work_calls, nonempty_calls)
            && !add_would_overflow(simd_eight_lane_calls, simd_sixteen_lane_calls)
            && !add_would_overflow(serial_align_region_dispatches,
                                   parallel_align_region_dispatches);
        if (!totals_valid || is_saturated) return false;
        requested_xdrop_total = left_eligible + right_eligible;
        executed_xdrop_total = left_executed + right_executed;
        requested_forward_total = gap_eligible + right_eligible;
        executed_forward_total = gap_executed + right_executed;
        work_total = zero_work_calls + nonempty_calls;
        lane_total = simd_eight_lane_calls + simd_sixteen_lane_calls;
        if (simd_diagonal_certificate_calls > nonempty_calls) return false;
        const std::uint64_t dp_nonempty_calls = nonempty_calls
            - simd_diagonal_certificate_calls;
        const bool exact_align_region_dispatch =
            serial_align_region_dispatches
                    + parallel_align_region_dispatches
                == align_region_invocations
            && (routed_region_scopes == 0 || align_region_invocations > 0);
        const bool exact_stage_coverage = left_eligible == left_executed
            && gap_eligible == gap_executed
            && right_eligible == right_executed;
        const bool exact_router_coverage = routed_region_scopes == 0
            || (left_route_candidates == left_executed
                && gap_route_candidates == gap_executed
                && right_route_candidates == right_executed);
        const bool exact_mode_coverage = global_calls == gap_eligible
            && native_x_drop_calls == requested_xdrop_total
            && executed_global_calls == gap_executed
            && executed_native_x_drop_calls == executed_xdrop_total;
        const bool exact_input_order_coverage =
            reversed_input_calls == left_eligible
            && forward_input_calls == requested_forward_total
            && executed_reversed_input_calls == left_executed
            && executed_forward_input_calls == executed_forward_total;
        const bool exact_config_coverage =
            configuration_identity_calls == eligible_total
            && executed_configuration_identity_calls == executed_total
            && configuration_identity_calls == executed_configuration_identity_calls
            && configuration_identity_xor == executed_configuration_identity_xor
            && configuration_identity_sum == executed_configuration_identity_sum;
        const bool truthful_dp_work =
            (dp_nonempty_calls == 0
                && simd_dp_vector_batches == 0 && simd_dp_cells == 0)
            || (dp_nonempty_calls > 0
                && simd_dp_vector_batches > 0 && simd_dp_cells > 0);
        const bool truthful_certificate_work =
            (simd_diagonal_certificate_calls == 0
                && simd_diagonal_certificate_vector_batches == 0
                && simd_diagonal_certificate_symbols == 0)
            || (simd_diagonal_certificate_calls > 0
                && simd_diagonal_certificate_vector_batches
                    >= simd_diagonal_certificate_calls
                && simd_diagonal_certificate_symbols
                    >= simd_diagonal_certificate_calls
                && simd_diagonal_certificate_vector_batches
                    <= simd_diagonal_certificate_symbols
                && simd_diagonal_certificate_calls
                    <= executed_global_calls);
        const bool truthful_work = work_total == executed_total
            && nonempty_calls_with_simd == nonempty_calls
            && lane_total == executed_total
            && truthful_dp_work
            && truthful_certificate_work;
        return exact_stage_coverage
            && exact_align_region_dispatch
            && exact_router_coverage
            && exact_mode_coverage
            && exact_input_order_coverage
            && exact_config_coverage
            && truthful_work
            && trace_calls == executed_trace_calls
            && expected_reversed_compact_symbols == reversed_compact_symbols
            && native_x_drop_terminations <= executed_native_x_drop_calls
            && unsupported_contract_rejections == 0
            && forbidden_ksw2_calls == 0
            && forbidden_scalar_biological_fallbacks == 0;
    }

    /** Compare every counter and saturation state. */
    [[nodiscard]] constexpr auto operator==(const block_aligner_execution_stats&) const noexcept
        -> bool = default;
};
/** Score and edit count for a gap whose diagonal is proved to be its unique best path. */
struct block_diagonal_certificate {
    bool is_certified = false;  ///< Whether the diagonal is the unique optimum.
    int32_t score = 0;  ///< Exact diagonal score.
    std::uint64_t edit_distance = 0;  ///< Mismatches along the certified diagonal.
    std::uint64_t vector_batches = 0;  ///< SIMD scan batches used.
    std::uint64_t symbols = 0;  ///< Symbols examined by the scan.
};

/** Count alignment-arena use and Block Aligner buffer allocations. */
struct block_aligner_construction_stats {
    std::uint64_t align_arena_scopes = 0;  ///< Alignment arena scopes entered.
    std::uint64_t padded_sequence_allocations = 0;  ///< Padded inputs allocated.
    std::uint64_t scratch_allocations = 0;  ///< DP scratch allocations.
    std::uint64_t trace_allocations = 0;  ///< Traceback allocations.
};
/** Parameters checked before a Block Aligner call starts. */
struct block_aligner_parameters {
    int8_t gap_open = 0;  ///< Positive gap-open penalty q. A gap of length l costs q + e*l.
    int8_t gap_extend = 0;  ///< Positive penalty e for each symbol in a gap.
    size_t minimum_block_size = 32;  ///< Minimum adaptive block size.
    size_t maximum_block_size = 256;  ///< Maximum adaptive block size.
    int32_t x_drop_threshold = 0;  ///< Allowed score loss during X-drop extension. Use zero in global mode.
    block_alignment_mode mode = block_alignment_mode::global;  ///< Global gap fill or X-drop extension.
    block_input_order input_order = block_input_order::forward;  ///< Forward or reversed interval order.
    block_alignment_stage stage = block_alignment_stage::gap_extension;  ///< Left flank, inter-seed gap or right flank.
    bool is_trace_enabled = true;  ///< Whether traceback is requested.
    bool should_compute_edit_distance = false;  ///< Count mismatches and gap bases. Requires traceback.
};

/**
 * Result of trying the packed diagonal check.
 * not_eligible allows the byte-based check to try. rejected means the packed
 * scan already ran and failed its score bound. certified supplies the result.
 */
enum class block_packed_certificate_decision : std::uint8_t {
    not_eligible,
    rejected,
    certified,
};

/// Prepare scoring and parameters once for a region's packed DNA4 gap checks.
/// Each gap supplies its own coordinates and sequence data. Keeping the shared
/// checks here avoids repeating them for every adjacent seed pair.
struct block_global_gap_certificate_plan {
    block_aligner_parameters parameters{};  ///< Global gap parameters to use when is_contract_supported is true.
    std::int8_t match_score = 0;  ///< Score for matching DNA4 bases.
    std::int8_t mismatch_score = 0;  ///< Score for mismatched DNA4 bases.
    std::uint64_t scoring_identity = 0;  ///< Hash identifying the scoring rules.
    /// Number of int16 lanes available on the selected CPU target.
    std::uint8_t simd_lane_count = 0;
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
    /// Number of byte lanes used by KSW2 on the selected CPU target.
    std::size_t ksw2_byte_lane_count = 0u;
    /// Bytes per accumulated KSW2 score on the selected CPU target.
    std::size_t ksw2_full_score_bytes = 0u;
#endif
    bool is_contract_supported = false;  ///< Whether the global gap parameters passed validation.
    bool is_generated_matrix_supported = false;  ///< Whether the matrix supports the diagonal score bound.
};

/** Packed-check decision and the result available when it succeeds. */
struct block_packed_certificate_attempt {
    /** Whether the packed check could run and prove the diagonal best. */
    block_packed_certificate_decision decision =
        block_packed_certificate_decision::not_eligible;
    block_diagonal_certificate certificate{};  ///< Exact result when certified.
};

/** Why Block Aligner rejected a call. */
enum class block_aligner_contract_error : std::uint8_t {
    none,
    gap_penalty,
    block_geometry,
    x_drop_threshold,
    stage_mode,
    lane_shape,
    coordinates,
    scoring_matrix,
    trace_accounting,
};

/** Return whether Block Aligner supports this number of SIMD lanes. */
[[nodiscard]] constexpr auto is_block_aligner_lane_shape_supported(size_t lanes) noexcept -> bool {
    return lanes == 8 || lanes == 16;
}



/** Buffer sizes needed for a single- or dual-affine KSW2 fill. */
struct ksw2_storage_plan {
    std::size_t vector_bytes = 0u;
    std::size_t target_vector_count = 0u;
    std::size_t query_vector_count = 0u;
    std::size_t state_array_count = 0u;
    std::size_t state_array_bytes = 0u;
    std::size_t state_bytes = 0u;
    std::size_t exact_score_elements = 0u;
    std::size_t exact_score_bytes = 0u;
    std::size_t anti_diagonal_count = 0u;
    std::size_t band_vector_capacity = 0u;
    std::size_t backtrack_vector_slots = 0u;
    std::size_t backtrack_bytes = 0u;
    std::size_t band_bound_elements = 0u;
    std::size_t packed_cigar_step_bound = 0u;
    std::size_t packed_cigar_capacity_bound = 0u;
    std::uint64_t packed_cigar_bytes = 0u;
    std::uint64_t logical_alignment_bytes = 0u;
};

/** Calculate KSW2 buffer sizes with overflow checks. Return nullopt for unsupported geometry. */
[[nodiscard]] constexpr std::optional<ksw2_storage_plan> derive_ksw2_storage_plan(
    std::size_t query_length, std::size_t reference_length, std::size_t vector_bytes,
    bool is_dual_affine, std::int32_t configured_band_width,
    bool exact_max_tracking, std::size_t full_score_bytes, bool compute_cigar) noexcept {
    constexpr auto int_limit =
        static_cast<std::size_t>((std::numeric_limits<std::int32_t>::max)());
    const auto steps = bioseq::common::checked_add(query_length, reference_length);
    if (!steps || query_length == 0u || reference_length == 0u ||
        query_length > int_limit || reference_length > int_limit ||
        *steps - 1u > int_limit || vector_bytes == 0u || vector_bytes > int_limit ||
        (exact_max_tracking && full_score_bytes == 0u)) {
        return std::nullopt;
    }
    const auto target_vectors = bioseq::common::ceil_div(reference_length, vector_bytes);
    const auto query_vectors = bioseq::common::ceil_div(query_length, vector_bytes);
    const std::size_t state_arrays = is_dual_affine ? 7u : 5u;
    const auto target_slots = bioseq::common::checked_mul(state_arrays + 1u, target_vectors);
    const auto with_query = target_slots
        ? bioseq::common::checked_add(*target_slots, query_vectors)
        : std::optional<std::size_t>{};
    const auto slots = with_query
        ? bioseq::common::checked_add(*with_query, std::size_t{1u})
        : std::optional<std::size_t>{};
    const auto state_bytes = slots
        ? bioseq::common::checked_mul(*slots, vector_bytes)
        : std::optional<std::size_t>{};
    const auto array_bytes = bioseq::common::checked_mul(target_vectors, vector_bytes);
    const auto exact_bytes = array_bytes
        ? bioseq::common::checked_mul(exact_max_tracking ? *array_bytes : 0u, full_score_bytes)
        : std::optional<std::size_t>{};
    const auto initial_bytes = state_bytes && exact_bytes
        ? bioseq::common::checked_add(*state_bytes, *exact_bytes)
        : std::optional<std::size_t>{};
    if (!initial_bytes || target_vectors > int_limit || query_vectors > int_limit) {
        return std::nullopt;
    }
    ksw2_storage_plan plan{
        .vector_bytes = vector_bytes,
        .target_vector_count = target_vectors,
        .query_vector_count = query_vectors,
        .state_array_count = state_arrays,
        .state_array_bytes = *array_bytes,
        .state_bytes = *state_bytes,
        .exact_score_elements = exact_max_tracking ? *array_bytes : 0u,
        .exact_score_bytes = *exact_bytes,
        .anti_diagonal_count = *steps - 1u,
        .logical_alignment_bytes = *initial_bytes,
    };
    const auto band_width = configured_band_width < 0
        ? std::max(query_length, reference_length)
        : static_cast<std::size_t>(configured_band_width);
    const auto band_plus_one = bioseq::common::checked_add(band_width, std::size_t{1u});
    const auto band_vectors = band_plus_one
        ? bioseq::common::checked_add(bioseq::common::ceil_div(
            std::min(std::min(query_length, reference_length), *band_plus_one),
            vector_bytes), std::size_t{1u})
        : std::optional<std::size_t>{};
    const auto band_row_bytes = band_vectors
        ? bioseq::common::checked_mul(*band_vectors, vector_bytes)
        : std::optional<std::size_t>{};
    if (!band_vectors || !band_row_bytes || *band_vectors > int_limit ||
        *band_row_bytes > int_limit) {
        return std::nullopt;
    }
    plan.band_vector_capacity = *band_vectors;
    if (!compute_cigar) {
        return plan;
    }
    const auto trace_slots = bioseq::common::checked_mul(plan.anti_diagonal_count, *band_vectors);
    const auto guarded_slots = trace_slots
        ? bioseq::common::checked_add(*trace_slots, std::size_t{1u})
        : std::optional<std::size_t>{};
    const auto trace_bytes = guarded_slots
        ? bioseq::common::checked_mul(*guarded_slots, vector_bytes)
        : std::optional<std::size_t>{};
    const auto band_bounds = bioseq::common::checked_mul(plan.anti_diagonal_count, std::size_t{2u});
    const auto band_bound_bytes = band_bounds
        ? bioseq::common::checked_mul(*band_bounds, sizeof(std::int32_t))
        : std::optional<std::size_t>{};
    const std::size_t cigar_capacity_bound = std::max(*steps, std::size_t{64u});
    const auto cigar_bytes = bioseq::common::checked_mul(
        cigar_capacity_bound, sizeof(std::uint32_t));
    const auto trace_total = trace_bytes && band_bound_bytes
        ? bioseq::common::checked_add(*trace_bytes, *band_bound_bytes)
        : std::optional<std::size_t>{};
    const auto total = trace_total
        ? bioseq::common::checked_add(*initial_bytes, *trace_total)
        : std::optional<std::size_t>{};
    if (!total || !cigar_bytes) {
        return std::nullopt;
    }
    plan.backtrack_vector_slots = *guarded_slots;
    plan.backtrack_bytes = *trace_bytes;
    plan.band_bound_elements = *band_bounds;
    plan.packed_cigar_step_bound = *steps;
    plan.packed_cigar_capacity_bound = cigar_capacity_bound;
    plan.packed_cigar_bytes = *cigar_bytes;
    plan.logical_alignment_bytes = *total;
    return plan;
}

/** Buffer sizes used when allocating Block Aligner scratch space. */
struct block_aligner_storage_plan {
    std::size_t query_padded_bytes = 0u;
    std::size_t reference_padded_bytes = 0u;
    std::size_t border_elements = 0u;
    std::size_t shift_elements = 64u;
    std::size_t trace_capacity = 0u;
    std::size_t trace_word_elements = 0u;
    std::size_t direction_word_elements = 0u;
    std::size_t coordinate_elements = 0u;
    std::size_t packed_cigar_step_bound = 0u;
    std::uint64_t logical_alignment_bytes = 0u;
    std::uint64_t packed_cigar_bytes = 0u;
    bool is_trace_enabled = false;
};

/** Size a padded sequence buffer, including its sentinel and padding for SIMD loads. */
[[nodiscard]] constexpr std::optional<std::size_t> block_padded_storage_elements(
    std::size_t length, std::size_t block_size) noexcept {
    const auto body = bioseq::common::checked_add(length, block_size);
    return body ? bioseq::common::checked_add(*body, std::size_t{65u})
                : std::optional<std::size_t>{};
}

/**
 * Calculate Block Aligner buffer sizes and reject dimensions that overflow.
 * The caller still needs to reserve and allocate the memory.
 */
[[nodiscard]] constexpr std::optional<block_aligner_storage_plan>
derive_block_aligner_storage_plan(
    std::size_t query_length, std::size_t reference_length,
    std::size_t maximum_block_size, std::size_t lane_count,
    bool is_trace_enabled) noexcept {
    if (!is_block_aligner_lane_shape_supported(lane_count) ||
        maximum_block_size < lane_count || maximum_block_size % lane_count != 0u) {
        return std::nullopt;
    }
    const auto query_padded = block_padded_storage_elements(query_length, maximum_block_size);
    const auto reference_padded = block_padded_storage_elements(reference_length, maximum_block_size);
    const auto sequence_steps = bioseq::common::checked_add(query_length, reference_length);
    const auto border_elements = bioseq::common::checked_mul(maximum_block_size, std::size_t{8u});
    const auto border_and_shift = border_elements
        ? bioseq::common::checked_add(*border_elements, std::size_t{128u})
        : std::optional<std::size_t>{};
    const auto border_bytes = border_and_shift
        ? bioseq::common::checked_mul(*border_and_shift, sizeof(block_aligner_score_type))
        : std::optional<std::size_t>{};
    const auto padded_bytes = query_padded && reference_padded
        ? bioseq::common::checked_add(*query_padded, *reference_padded)
        : std::optional<std::size_t>{};
    const auto initial_bytes = padded_bytes && border_bytes
        ? bioseq::common::checked_add(*padded_bytes, *border_bytes)
        : std::optional<std::size_t>{};
    if (!initial_bytes || !sequence_steps) {
        return std::nullopt;
    }
    block_aligner_storage_plan plan{
        .query_padded_bytes = *query_padded,
        .reference_padded_bytes = *reference_padded,
        .border_elements = maximum_block_size,
        .logical_alignment_bytes = *initial_bytes,
        .is_trace_enabled = is_trace_enabled,
    };
    if (!is_trace_enabled) {
        return plan;
    }
    const auto combined = bioseq::common::checked_add(*sequence_steps, std::size_t{2u});
    const auto double_block = bioseq::common::checked_mul(maximum_block_size, std::size_t{2u});
    const auto columns = combined && double_block
        ? bioseq::common::checked_add(*combined, *double_block)
        : std::optional<std::size_t>{};
    const auto trace_capacity = columns
        ? bioseq::common::checked_mul(maximum_block_size / lane_count, *columns)
        : std::optional<std::size_t>{};
    const auto trace_words = trace_capacity
        ? bioseq::common::checked_add(*trace_capacity, std::size_t{64u})
        : std::optional<std::size_t>{};
    const auto direction_bits = combined
        ? bioseq::common::checked_mul(*combined, std::size_t{2u})
        : std::optional<std::size_t>{};
    const auto coordinates = combined
        ? bioseq::common::checked_mul(*combined, std::size_t{4u})
        : std::optional<std::size_t>{};
    const auto trace_bytes = trace_words
        ? bioseq::common::checked_mul(*trace_words, std::size_t{2u * sizeof(std::uint32_t)})
        : std::optional<std::size_t>{};
    const auto coordinate_bytes = coordinates
        ? bioseq::common::checked_mul(*coordinates,
            std::size_t{sizeof(std::uint32_t) + sizeof(std::uint16_t)})
        : std::optional<std::size_t>{};
    const auto cigar_bytes = bioseq::common::checked_mul(*sequence_steps, sizeof(std::uint32_t));
    if (!trace_bytes || !direction_bits || !coordinate_bytes || !cigar_bytes) {
        return std::nullopt;
    }
    const auto direction_words = bioseq::common::ceil_div(*direction_bits, std::size_t{64u});
    const auto direction_bytes = bioseq::common::checked_mul(direction_words, sizeof(std::uint64_t));
    const auto traces_and_coordinates = bioseq::common::checked_add(*trace_bytes, *coordinate_bytes);
    const auto trace_total = direction_bytes && traces_and_coordinates
        ? bioseq::common::checked_add(*direction_bytes, *traces_and_coordinates)
        : std::optional<std::size_t>{};
    const auto total = trace_total
        ? bioseq::common::checked_add(*initial_bytes, *trace_total)
        : std::optional<std::size_t>{};
    if (!total) {
        return std::nullopt;
    }
    plan.trace_capacity = *trace_capacity;
    plan.trace_word_elements = *trace_words;
    plan.direction_word_elements = direction_words;
    plan.coordinate_elements = *coordinates;
    plan.packed_cigar_step_bound = *sequence_steps;
    plan.logical_alignment_bytes = *total;
    plan.packed_cigar_bytes = *cigar_bytes;
    return plan;
}

/** Check gap costs, block sizes, extension mode and SIMD width before alignment. */
[[nodiscard]] constexpr auto
validate_block_aligner_contract(const block_aligner_parameters& parameters, size_t lanes) noexcept
    -> block_aligner_contract_error {
    if (parameters.gap_open <= 0 || parameters.gap_extend <= 0
        || static_cast<int>(parameters.gap_open) + parameters.gap_extend
             > -static_cast<int>(std::numeric_limits<int8_t>::min())) {
        return block_aligner_contract_error::gap_penalty;
    }
    if (!is_block_aligner_lane_shape_supported(lanes)) {
        return block_aligner_contract_error::lane_shape;
    }
    const size_t minimum_block_size = std::max(parameters.minimum_block_size, lanes);
    const size_t maximum_block_size = std::max(parameters.maximum_block_size, lanes);
    if (!std::has_single_bit(minimum_block_size) || !std::has_single_bit(maximum_block_size)
        || minimum_block_size > maximum_block_size
        || maximum_block_size >= std::numeric_limits<std::uint16_t>::max()) {
        return block_aligner_contract_error::block_geometry;
    }
    if ((parameters.mode == block_alignment_mode::x_drop && parameters.x_drop_threshold < 0)
        || (parameters.mode == block_alignment_mode::global && parameters.x_drop_threshold != 0)) {
        return block_aligner_contract_error::x_drop_threshold;
    }
    const bool stage_mode_valid =
        (parameters.stage == block_alignment_stage::left_extension
            && parameters.mode == block_alignment_mode::x_drop
            && parameters.input_order == block_input_order::reversed)
        || (parameters.stage == block_alignment_stage::gap_extension
            && parameters.mode == block_alignment_mode::global
            && parameters.input_order == block_input_order::forward)
        || (parameters.stage == block_alignment_stage::right_extension
            && parameters.mode == block_alignment_mode::x_drop
            && parameters.input_order == block_input_order::forward);
    if (!stage_mode_valid) {
        return block_aligner_contract_error::stage_mode;
    }
    if (parameters.should_compute_edit_distance && !parameters.is_trace_enabled) {
        return block_aligner_contract_error::trace_accounting;
    }
    return block_aligner_contract_error::none;
}

/** Hash the alignment parameters, scoring rules and query strand for execution checks. */
[[nodiscard]] constexpr auto
block_aligner_configuration_identity(const block_aligner_parameters& parameters,
                                     std::uint64_t scoring_identity = 0,
                                     bool strand_reversed = false) noexcept -> std::uint64_t {
    std::uint64_t hash = 1469598103934665603ULL;
    const auto mix = [&hash](std::uint64_t value) constexpr -> void {
        hash ^= value;
        hash *= 1099511628211ULL;
    };
    mix(static_cast<std::uint8_t>(parameters.gap_open));
    mix(static_cast<std::uint8_t>(parameters.gap_extend));
    mix(parameters.minimum_block_size);
    mix(parameters.maximum_block_size);
    mix(static_cast<std::uint32_t>(parameters.x_drop_threshold));
    mix(static_cast<std::uint8_t>(parameters.mode));
    mix(static_cast<std::uint8_t>(parameters.input_order));
    mix(static_cast<std::uint8_t>(parameters.stage));
    mix(parameters.is_trace_enabled);
    mix(parameters.should_compute_edit_distance);
    mix(scoring_identity);
    mix(strand_reversed);
    return hash;
}

/** Count a certified gap as completed work without calling the SIMD kernel again. */
inline auto
record_block_certified_global_gap(const block_global_gap_certificate_plan& plan,
                                  const block_diagonal_certificate& certificate,
                                  bool strand_reversed,
                                  block_aligner_execution_stats* execution_stats) noexcept -> void {
    if (execution_stats == nullptr) return;
    if (!certificate.is_certified || certificate.symbols == 0u
        || certificate.vector_batches == 0u
        || certificate.edit_distance > certificate.symbols
        || !is_block_aligner_lane_shape_supported(plan.simd_lane_count)) {
        std::abort();
    }

    auto& stats = *execution_stats;
    const std::uint64_t identity = block_aligner_configuration_identity(
        plan.parameters, plan.scoring_identity, strand_reversed);

    stats.add(stats.gap_eligible);
    stats.add(stats.global_calls);
    if (plan.parameters.is_trace_enabled) stats.add(stats.trace_calls);
    stats.add(stats.forward_input_calls);
    stats.add(stats.configuration_identity_calls);
    stats.configuration_identity_xor ^= identity;
    stats.configuration_identity_sum += identity;

    stats.add(stats.gap_executed);
    stats.add(stats.executed_global_calls);
    if (plan.parameters.is_trace_enabled) stats.add(stats.executed_trace_calls);
    stats.add(stats.executed_forward_input_calls);
    if (plan.simd_lane_count == 8u) stats.add(stats.simd_eight_lane_calls);
    else stats.add(stats.simd_sixteen_lane_calls);
    stats.add(stats.nonempty_calls);
    stats.add(stats.nonempty_calls_with_simd);
    stats.add(stats.simd_diagonal_certificate_calls);
    stats.add(stats.simd_diagonal_certificate_vector_batches,
        certificate.vector_batches);
    stats.add(stats.simd_diagonal_certificate_symbols,
        certificate.symbols);
    stats.add(stats.executed_configuration_identity_calls);
    stats.executed_configuration_identity_xor ^= identity;
    stats.executed_configuration_identity_sum += identity;
}

/** Record and abort an attempt to fall back from Block Aligner to KSW2. */
[[noreturn]] inline auto
abort_forbidden_ksw2_fallback(block_aligner_execution_stats& stats) noexcept -> void {
    stats.add(stats.forbidden_ksw2_calls);
    std::abort();
}

}  // namespace bioseq::simd
