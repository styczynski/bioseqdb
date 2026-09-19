/**
 * @file
 * @brief Target-neutral parameters for one minimap-compatible alignment request.
 */
module;

#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <mutex>
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
#include <memory>
#endif

export module bioseq.pipeline.minimap_parameters_contract;

export import bioseq.pipeline.runtime_contract;
export import bioseq.chaining.core;
export import bioseq.highway.kernel_table_contract;
import bioseq.instrumentation.clock;

export namespace bioseq::simd {

using bioseq::chaining::chaining_mode;
using bioseq::chaining::chaining_parameters;
using bioseq::chaining::simd_chaining_policy;

#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
/** Executor shape used only to select the guarded statement-plan proof contract. */
enum class benchmark_e2e_statement_plan_shape : std::uint8_t {
    column_scan,
    per_row_join,
    batch_join,
};

/** Guarded control-block handoffs; these values are never serialized as metrics. */
enum class benchmark_e2e_statement_plan_checkpoint : std::uint8_t {
    admission,
    scan_dispatch,
    join_preparation,
    query_union,
    statistics,
    pipeline,
    retry_request,
    retry_reentry,
};
#endif

#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
/** Work classes whose physical interval union is excluded from outer E2E wall. */
enum class benchmark_e2e_interval_kind : std::uint8_t {
    cigar,
    mapq,
};

/** Authentic producer attached to one benchmark-only physical interval. */
enum class benchmark_e2e_interval_algorithm : std::uint8_t {
    ksw2,
    block_aligner,
    vulkan_simple_cigar,
    cpu_mapping_quality,
    vulkan_mapping_quality,
};

[[nodiscard]] constexpr const char*
benchmark_e2e_interval_kind_name(const benchmark_e2e_interval_kind kind) noexcept {
    return kind == benchmark_e2e_interval_kind::cigar ? "cigar" : "mapq";
}

[[nodiscard]] constexpr const char*
benchmark_e2e_interval_algorithm_name(const benchmark_e2e_interval_algorithm algorithm) noexcept {
    switch (algorithm) {
    case benchmark_e2e_interval_algorithm::ksw2:
        return "ksw2";
    case benchmark_e2e_interval_algorithm::block_aligner:
        return "block_aligner";
    case benchmark_e2e_interval_algorithm::vulkan_simple_cigar:
        return "vulkan_simple_cigar";
    case benchmark_e2e_interval_algorithm::cpu_mapping_quality:
        return "cpu_mapping_quality";
    case benchmark_e2e_interval_algorithm::vulkan_mapping_quality:
        return "vulkan_mapping_quality";
    }
    return "invalid";
}

/** Truthful report label for inseparable work inside the physical interval. */
[[nodiscard]] constexpr const char*
benchmark_e2e_inclusive_work_label(const benchmark_e2e_interval_algorithm algorithm) noexcept {
    switch (algorithm) {
    case benchmark_e2e_interval_algorithm::ksw2:
        return "alignment_dp_trace_storage_traceback_cigar_assembly_result_materialization_"
               "inclusive";
    case benchmark_e2e_interval_algorithm::block_aligner:
        return "block_dp_trace_storage_traceback_cigar_assembly_result_materialization_inclusive";
    case benchmark_e2e_interval_algorithm::vulkan_simple_cigar:
        return "host_packing_upload_gpu_alignment_traceback_wait_readback_decode_result_"
               "materialization_inclusive";
    case benchmark_e2e_interval_algorithm::cpu_mapping_quality:
        return "compute_mapping_quality_only";
    case benchmark_e2e_interval_algorithm::vulkan_mapping_quality:
        return "host_validation_upload_gpu_classification_mapq_wait_readback_publication_inclusive";
    }
    return "invalid";
}

/** Stable logical operation identity within one statement-owned tracker. */
struct benchmark_e2e_operation_origin {
    std::uint64_t logical_target_row = 0;
    std::uint64_t query_ordinal = 0;

    [[nodiscard]] constexpr bool
    operator==(const benchmark_e2e_operation_origin&) const noexcept = default;
};

/** Existing per-job attribution, independent of immutable biological parameters. */
struct benchmark_e2e_operation_binding {
    std::weak_ptr<const void> statement_owner{};
    benchmark_e2e_operation_origin origin{};
    bool is_available = false;
};

namespace benchmark_e2e_detail {
inline thread_local benchmark_e2e_operation_binding active_operation{};
}

/** Capture attribution by value before retaining a graph or scheduling a worker. */
[[nodiscard, gnu::always_inline]] inline benchmark_e2e_operation_binding
current_benchmark_e2e_operation_binding() noexcept {
    return benchmark_e2e_detail::active_operation;
}

/**
 * Install one captured job binding for synchronous work on this thread.
 * Worker closures must capture the value and install their own scope; the
 * thread-local slot itself is never treated as inherited worker state.
 */
class benchmark_e2e_operation_scope final {
    benchmark_e2e_operation_binding previous_;

  public:
    [[gnu::always_inline]] explicit benchmark_e2e_operation_scope(
        benchmark_e2e_operation_binding binding) noexcept
        : previous_(benchmark_e2e_detail::active_operation) {
        benchmark_e2e_detail::active_operation = binding;
    }
    benchmark_e2e_operation_scope(const benchmark_e2e_operation_scope&) = delete;
    benchmark_e2e_operation_scope& operator=(const benchmark_e2e_operation_scope&) = delete;
    [[gnu::always_inline]] ~benchmark_e2e_operation_scope() noexcept {
        benchmark_e2e_detail::active_operation = previous_;
    }
};

/** One origin's participation in physical intervals of one semantic kind. */
struct benchmark_e2e_interval_origin_summary {
    benchmark_e2e_operation_origin origin{};
    benchmark_e2e_interval_kind kind = benchmark_e2e_interval_kind::cigar;
    benchmark_e2e_interval_algorithm algorithm = benchmark_e2e_interval_algorithm::ksw2;
    std::uint64_t interval_count = 0;

    [[nodiscard]] constexpr bool
    operator==(const benchmark_e2e_interval_origin_summary&) const noexcept = default;
};

/** Fixed-layout finalized union receipt for one PostgreSQL statement. */
struct benchmark_e2e_interval_union_receipt {
    std::uint64_t union_ns = 0;
    std::uint64_t interval_count = 0;
    std::uint64_t cigar_interval_count = 0;
    std::uint64_t mapq_interval_count = 0;
    bool available = false;
    bool invalid = false;
    bool capacity_overflow = false;

    [[nodiscard]] constexpr bool
    operator==(const benchmark_e2e_interval_union_receipt&) const noexcept = default;
};

static_assert(__is_trivially_copyable(benchmark_e2e_interval_union_receipt));
static_assert(__is_standard_layout(benchmark_e2e_interval_union_receipt));

/**
 * Fixed-layout E2E evidence buffer. Overflow is never truncated: callers must
 * mark the enclosing receipt invalid and unavailable when try_push_back fails.
 */
template <class p_value, std::size_t p_capacity> struct benchmark_e2e_fixed_buffer {
    static_assert(p_capacity != 0u);
    static_assert(__is_trivially_copyable(p_value));
    static_assert(__is_standard_layout(p_value));

  private:
    p_value values_[p_capacity]{};
    std::size_t size_ = 0;

    constexpr void swap_values(const std::size_t lhs, const std::size_t rhs) noexcept {
        p_value held = values_[lhs];
        values_[lhs] = values_[rhs];
        values_[rhs] = held;
    }

    template <class p_less>
    constexpr void sift_down(std::size_t root, const std::size_t count, p_less& less) noexcept {
        for (;;) {
            const std::size_t left_child = root * 2u + 1u;
            if (left_child >= count) {
                return;
            }
            std::size_t largest = root;
            if (less(values_[largest], values_[left_child])) {
                largest = left_child;
            }
            const std::size_t right_child = left_child + 1u;
            if (right_child < count && less(values_[largest], values_[right_child])) {
                largest = right_child;
            }
            if (largest == root) {
                return;
            }
            swap_values(root, largest);
            root = largest;
        }
    }

  public:
    [[nodiscard]] constexpr p_value* begin() noexcept {
        return values_;
    }
    [[nodiscard]] constexpr const p_value* begin() const noexcept {
        return values_;
    }
    [[nodiscard]] constexpr p_value* end() noexcept {
        return values_ + size_;
    }
    [[nodiscard]] constexpr const p_value* end() const noexcept {
        return values_ + size_;
    }
    [[nodiscard]] constexpr bool empty() const noexcept {
        return size_ == 0u;
    }
    [[nodiscard]] constexpr std::size_t size() const noexcept {
        return size_;
    }
    [[nodiscard]] static constexpr std::size_t capacity() noexcept {
        return p_capacity;
    }
    [[nodiscard]] constexpr p_value& front() noexcept {
        return values_[0];
    }
    [[nodiscard]] constexpr const p_value& front() const noexcept {
        return values_[0];
    }
    [[nodiscard]] constexpr p_value& operator[](const std::size_t index) noexcept {
        return values_[index];
    }
    [[nodiscard]] constexpr const p_value& operator[](const std::size_t index) const noexcept {
        return values_[index];
    }

    [[nodiscard]] constexpr bool try_push_back(const p_value& value) noexcept {
        if (size_ == p_capacity) {
            return false;
        }
        values_[size_] = value;
        ++size_;
        return true;
    }

    constexpr void erase_at(const std::size_t index) noexcept {
        if (index >= size_) {
            return;
        }
        for (std::size_t source = index + 1u; source < size_; ++source) {
            values_[source - 1u] = values_[source];
        }
        --size_;
    }

    template <class p_less> constexpr void sort(p_less less) noexcept {
        if (size_ < 2u) {
            return;
        }
        for (std::size_t parent = size_ / 2u; parent != 0u; --parent) {
            sift_down(parent - 1u, size_, less);
        }
        for (std::size_t remaining = size_; remaining > 1u; --remaining) {
            swap_values(0u, remaining - 1u);
            sift_down(0u, remaining - 1u, less);
        }
    }
};

// The sealed matrix owns 1,024 query origins and can require one CIGAR plus one
// MAPQ summary for each. These E2E-only bounds retain at least twofold origin
// headroom and ample per-region physical-interval headroom; exceeding any bound
// invalidates the receipt instead of truncating evidence.
inline constexpr std::size_t k_benchmark_e2e_live_interval_capacity = 4096u;
inline constexpr std::size_t k_benchmark_e2e_live_origin_capacity = 4096u;
inline constexpr std::size_t k_benchmark_e2e_completed_interval_capacity = 131072u;
inline constexpr std::size_t k_benchmark_e2e_origin_summary_capacity = 4096u;
static_assert(k_benchmark_e2e_origin_summary_capacity >= 2u * 1024u);

using benchmark_e2e_interval_origin_buffer =
    benchmark_e2e_fixed_buffer<benchmark_e2e_interval_origin_summary,
                               k_benchmark_e2e_origin_summary_capacity>;

static_assert(__is_trivially_copyable(benchmark_e2e_interval_origin_buffer));
static_assert(__is_standard_layout(benchmark_e2e_interval_origin_buffer));

/** E2E-only fixed-layout sidecar for operation-origin proof. */
struct benchmark_e2e_interval_evidence {
    benchmark_e2e_interval_union_receipt interval_union{};
    benchmark_e2e_interval_origin_buffer origins{};
};
#endif

/** One finalized PostgreSQL-statement union of active downstream intervals. */
struct downstream_active_wall_receipt {
    std::uint64_t elapsed_ns = 0;
    bool available = false;
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    benchmark_e2e_interval_evidence benchmark_e2e_intervals{};
#endif
};

/**
 * Thread-safe union accumulator shared by every query, segment, and worker
 * owned by one PostgreSQL statement. The statement owner finalizes it once,
 * only after all tracked work has joined.
 */
class downstream_active_wall_tracker final {
  private:
    std::mutex mutex_{};
    std::uint64_t active_count_ = 0;
    std::uint64_t active_interval_started_ns_ = 0;
    std::uint64_t accumulated_ns_ = 0;
    bool observed_interval_ = false;
    bool invalid_ = false;
    bool finalized_ = false;
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
    // Guarded control-block proof only; never serialized as a metric.
    std::weak_ptr<const void> benchmark_e2e_statement_owner_{};
    bool benchmark_e2e_statement_owner_bound_ = false;
    benchmark_e2e_statement_plan_shape benchmark_e2e_statement_plan_shape_ =
        benchmark_e2e_statement_plan_shape::column_scan;
    std::uint16_t benchmark_e2e_statement_plan_required_checkpoints_ = 0u;
    std::uint16_t benchmark_e2e_statement_plan_observed_checkpoints_ = 0u;
    bool benchmark_e2e_statement_plan_shape_bound_ = false;

    [[nodiscard]] static constexpr std::uint16_t benchmark_e2e_statement_plan_checkpoint_mask(
        const benchmark_e2e_statement_plan_checkpoint checkpoint) noexcept {
        return static_cast<std::uint16_t>(
            std::uint16_t{1u} << static_cast<std::uint8_t>(checkpoint));
    }

    [[nodiscard]] static constexpr std::uint16_t
    benchmark_e2e_statement_plan_required_checkpoints(
        const benchmark_e2e_statement_plan_shape shape) noexcept {
        const std::uint16_t common =
            benchmark_e2e_statement_plan_checkpoint_mask(
                benchmark_e2e_statement_plan_checkpoint::admission) |
            benchmark_e2e_statement_plan_checkpoint_mask(
                benchmark_e2e_statement_plan_checkpoint::scan_dispatch) |
            benchmark_e2e_statement_plan_checkpoint_mask(
                benchmark_e2e_statement_plan_checkpoint::statistics) |
            benchmark_e2e_statement_plan_checkpoint_mask(
                benchmark_e2e_statement_plan_checkpoint::pipeline);
        return shape == benchmark_e2e_statement_plan_shape::batch_join
                   ? static_cast<std::uint16_t>(
                         common |
                         benchmark_e2e_statement_plan_checkpoint_mask(
                             benchmark_e2e_statement_plan_checkpoint::join_preparation))
                   : common;
    }

    [[nodiscard]] bool benchmark_e2e_statement_owner_matches_locked(
        const std::shared_ptr<const void>& owner) const noexcept {
        const auto expected = benchmark_e2e_statement_owner_.lock();
        return benchmark_e2e_statement_owner_bound_ && expected && owner &&
               expected.get() == owner.get() &&
               !expected.owner_before(owner) && !owner.owner_before(expected);
    }

    void invalidate_benchmark_e2e_statement_plan_locked() noexcept {
        invalid_ = true;
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
        benchmark_e2e_invalid_ = true;
#endif
    }

    void validate_benchmark_e2e_statement_plan_locked() noexcept {
        if (!benchmark_e2e_statement_owner_bound_) {
            return;
        }
        const auto owner = benchmark_e2e_statement_owner_.lock();
        const bool required_checkpoints_observed =
            (benchmark_e2e_statement_plan_observed_checkpoints_ &
             benchmark_e2e_statement_plan_required_checkpoints_) ==
            benchmark_e2e_statement_plan_required_checkpoints_;
        const bool retry_reentered =
            (benchmark_e2e_statement_plan_observed_checkpoints_ &
             benchmark_e2e_statement_plan_checkpoint_mask(
                 benchmark_e2e_statement_plan_checkpoint::retry_request)) == 0u ||
            (benchmark_e2e_statement_plan_observed_checkpoints_ &
             benchmark_e2e_statement_plan_checkpoint_mask(
                 benchmark_e2e_statement_plan_checkpoint::retry_reentry)) != 0u;
        if (!owner || !benchmark_e2e_statement_plan_shape_bound_ ||
            !required_checkpoints_observed || !retry_reentered) {
            invalidate_benchmark_e2e_statement_plan_locked();
        }
    }
#endif

#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    struct benchmark_e2e_live_interval {
        std::uint64_t token = 0;
        std::uint64_t started_ns = 0;
        benchmark_e2e_interval_kind kind = benchmark_e2e_interval_kind::cigar;
        benchmark_e2e_interval_algorithm algorithm = benchmark_e2e_interval_algorithm::ksw2;
    };

    struct benchmark_e2e_completed_interval {
        std::uint64_t started_ns = 0;
        std::uint64_t finished_ns = 0;
    };

    struct benchmark_e2e_live_origin {
        std::uint64_t token = 0;
        benchmark_e2e_operation_origin origin{};
    };

    std::uint64_t benchmark_e2e_interval_count_ = 0;
    std::uint64_t benchmark_e2e_cigar_interval_count_ = 0;
    std::uint64_t benchmark_e2e_mapq_interval_count_ = 0;
    std::uint64_t benchmark_e2e_next_token_ = 1;
    bool benchmark_e2e_invalid_ = false;
    bool benchmark_e2e_capacity_overflow_ = false;
    benchmark_e2e_fixed_buffer<benchmark_e2e_live_interval, k_benchmark_e2e_live_interval_capacity>
        benchmark_e2e_live_intervals_{};
    benchmark_e2e_fixed_buffer<benchmark_e2e_live_origin, k_benchmark_e2e_live_origin_capacity>
        benchmark_e2e_live_origins_{};
    benchmark_e2e_fixed_buffer<benchmark_e2e_completed_interval,
                               k_benchmark_e2e_completed_interval_capacity>
        benchmark_e2e_completed_intervals_{};
    benchmark_e2e_interval_origin_buffer benchmark_e2e_origins_{};

    void benchmark_e2e_mark_capacity_overflow_locked() noexcept {
        benchmark_e2e_capacity_overflow_ = true;
        benchmark_e2e_invalid_ = true;
    }

    [[nodiscard]] static constexpr bool
    benchmark_e2e_origin_valid(const benchmark_e2e_operation_origin& origin) noexcept {
        constexpr std::uint64_t sql_bigint_max =
            static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());
        return origin.logical_target_row <= sql_bigint_max &&
               origin.query_ordinal <= sql_bigint_max;
    }

    [[nodiscard]] static constexpr bool benchmark_e2e_kind_matches_algorithm(
        const benchmark_e2e_interval_kind kind,
        const benchmark_e2e_interval_algorithm algorithm) noexcept {
        if (kind == benchmark_e2e_interval_kind::mapq) {
            return algorithm == benchmark_e2e_interval_algorithm::cpu_mapping_quality ||
                   algorithm == benchmark_e2e_interval_algorithm::vulkan_mapping_quality;
        }
        return algorithm == benchmark_e2e_interval_algorithm::ksw2 ||
               algorithm == benchmark_e2e_interval_algorithm::block_aligner ||
               algorithm == benchmark_e2e_interval_algorithm::vulkan_simple_cigar;
    }

    bool
    benchmark_e2e_record_origin_locked(const benchmark_e2e_operation_origin& origin,
                                       const benchmark_e2e_interval_kind kind,
                                       const benchmark_e2e_interval_algorithm algorithm) noexcept {
        for (benchmark_e2e_interval_origin_summary& summary : benchmark_e2e_origins_) {
            if (summary.origin == origin && summary.kind == kind &&
                summary.algorithm == algorithm) {
                if (summary.interval_count == (std::numeric_limits<std::uint64_t>::max)()) {
                    benchmark_e2e_invalid_ = true;
                    return false;
                }
                ++summary.interval_count;
                return true;
            }
        }
        if (!benchmark_e2e_origins_.try_push_back({
                .origin = origin,
                .kind = kind,
                .algorithm = algorithm,
                .interval_count = 1,
            })) {
            benchmark_e2e_mark_capacity_overflow_locked();
            return false;
        }
        return true;
    }
#endif

  public:
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
    /** Bind one completed immutable executor owner before any observation. */
    [[nodiscard]] bool bind_benchmark_e2e_statement_owner(
        const std::shared_ptr<const void>& owner) noexcept {
        std::scoped_lock lock{mutex_};
        if (!owner || finalized_) {
            invalidate_benchmark_e2e_statement_plan_locked();
            return false;
        }
        if (benchmark_e2e_statement_owner_bound_) {
            const bool matches = benchmark_e2e_statement_owner_matches_locked(owner);
            if (!matches) {
                invalidate_benchmark_e2e_statement_plan_locked();
            }
            return matches;
        }
        benchmark_e2e_statement_owner_ = owner;
        benchmark_e2e_statement_owner_bound_ = true;
        return true;
    }

    /** Check the existing observation's executor control block, without a metric. */
    [[nodiscard]] bool matches_benchmark_e2e_statement_owner(
        const std::shared_ptr<const void>& owner) noexcept {
        std::scoped_lock lock{mutex_};
        return benchmark_e2e_statement_owner_matches_locked(owner);
    }

    /** Bind admission and the route-appropriate required checkpoint set. */
    [[nodiscard]] bool begin_benchmark_e2e_statement_plan_proof(
        const std::shared_ptr<const void>& owner,
        const benchmark_e2e_statement_plan_shape shape) noexcept {
        std::scoped_lock lock{mutex_};
        if (finalized_ || !benchmark_e2e_statement_owner_matches_locked(owner) ||
            (benchmark_e2e_statement_plan_shape_bound_ &&
             benchmark_e2e_statement_plan_shape_ != shape)) {
            invalidate_benchmark_e2e_statement_plan_locked();
            return false;
        }
        benchmark_e2e_statement_plan_shape_ = shape;
        benchmark_e2e_statement_plan_shape_bound_ = true;
        benchmark_e2e_statement_plan_required_checkpoints_ =
            benchmark_e2e_statement_plan_required_checkpoints(shape);
        benchmark_e2e_statement_plan_observed_checkpoints_ |=
            benchmark_e2e_statement_plan_checkpoint_mask(
                benchmark_e2e_statement_plan_checkpoint::admission);
        return true;
    }

    /** Attest one executed handoff against the bound shared control block. */
    [[nodiscard]] bool attest_benchmark_e2e_statement_plan_checkpoint(
        const std::shared_ptr<const void>& owner,
        const benchmark_e2e_statement_plan_checkpoint checkpoint) noexcept {
        std::scoped_lock lock{mutex_};
        if (finalized_ || !benchmark_e2e_statement_plan_shape_bound_ ||
            !benchmark_e2e_statement_owner_matches_locked(owner)) {
            invalidate_benchmark_e2e_statement_plan_locked();
            return false;
        }
        benchmark_e2e_statement_plan_observed_checkpoints_ |=
            benchmark_e2e_statement_plan_checkpoint_mask(checkpoint);
        return true;
    }

#endif

    void enter() noexcept {
        std::scoped_lock lock{mutex_};
        const std::uint64_t started_ns =
            static_cast<std::uint64_t>(bioseq::instrumentation::clock::now_ns());
        if (finalized_ || active_count_ == (std::numeric_limits<std::uint64_t>::max)()) {
            invalid_ = true;
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
            benchmark_e2e_invalid_ = true;
#endif
            return;
        }
        if (active_count_ == 0u) {
            active_interval_started_ns_ = started_ns;
        }
        ++active_count_;
        observed_interval_ = true;
    }

    void leave() noexcept {
        std::scoped_lock lock{mutex_};
        const std::uint64_t finished_ns =
            static_cast<std::uint64_t>(bioseq::instrumentation::clock::now_ns());
        if (finalized_ || active_count_ == 0u || finished_ns < active_interval_started_ns_) {
            invalid_ = true;
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
            benchmark_e2e_invalid_ = true;
#endif
            return;
        }
        --active_count_;
        if (active_count_ != 0u)
            return;
        const std::uint64_t elapsed_ns = finished_ns - active_interval_started_ns_;
        if (elapsed_ns > (std::numeric_limits<std::uint64_t>::max)() - accumulated_ns_) {
            invalid_ = true;
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
            benchmark_e2e_invalid_ = true;
#endif
            accumulated_ns_ = (std::numeric_limits<std::uint64_t>::max)();
            return;
        }
        accumulated_ns_ += elapsed_ns;
    }

    void invalidate() noexcept {
        std::scoped_lock lock{mutex_};
        invalid_ = true;
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
        benchmark_e2e_invalid_ = true;
#endif
    }

#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    /** Begin one physical half-open interval and return its statement-local token. */
    [[nodiscard]] std::uint64_t
    begin_benchmark_e2e_interval(const benchmark_e2e_interval_kind kind,
                                 const benchmark_e2e_interval_algorithm algorithm,
                                 const benchmark_e2e_operation_origin& origin,
                                 const std::int64_t signed_started_ns) noexcept {
        std::scoped_lock lock{mutex_};
        if (finalized_ || benchmark_e2e_invalid_ || signed_started_ns <= 0 ||
            !benchmark_e2e_origin_valid(origin) ||
            !benchmark_e2e_kind_matches_algorithm(kind, algorithm) ||
            benchmark_e2e_interval_count_ == (std::numeric_limits<std::uint64_t>::max)() ||
            benchmark_e2e_next_token_ == (std::numeric_limits<std::uint64_t>::max)()) {
            benchmark_e2e_invalid_ = true;
            return 0;
        }

        const std::uint64_t token = benchmark_e2e_next_token_;
        const std::uint64_t started_ns = static_cast<std::uint64_t>(signed_started_ns);
        if (!benchmark_e2e_live_intervals_.try_push_back({
                .token = token,
                .started_ns = started_ns,
                .kind = kind,
                .algorithm = algorithm,
            })) {
            benchmark_e2e_mark_capacity_overflow_locked();
            return 0;
        }
        if (!benchmark_e2e_live_origins_.try_push_back({
                .token = token,
                .origin = origin,
            })) {
            benchmark_e2e_live_intervals_.erase_at(benchmark_e2e_live_intervals_.size() - 1u);
            benchmark_e2e_mark_capacity_overflow_locked();
            return 0;
        }
        if (!benchmark_e2e_record_origin_locked(origin, kind, algorithm)) {
            return 0;
        }
        ++benchmark_e2e_interval_count_;
        if (kind == benchmark_e2e_interval_kind::cigar) {
            if (benchmark_e2e_cigar_interval_count_ ==
                (std::numeric_limits<std::uint64_t>::max)()) {
                benchmark_e2e_invalid_ = true;
                return 0;
            }
            ++benchmark_e2e_cigar_interval_count_;
        } else {
            if (benchmark_e2e_mapq_interval_count_ == (std::numeric_limits<std::uint64_t>::max)()) {
                benchmark_e2e_invalid_ = true;
                return 0;
            }
            ++benchmark_e2e_mapq_interval_count_;
        }
        ++benchmark_e2e_next_token_;
        return token;
    }

    /** Attach another logical operation origin to an already-active batch interval. */
    void
    attach_benchmark_e2e_interval_origin(const std::uint64_t token,
                                         const benchmark_e2e_operation_origin& origin) noexcept {
        std::scoped_lock lock{mutex_};
        benchmark_e2e_live_interval* live = nullptr;
        for (benchmark_e2e_live_interval& candidate : benchmark_e2e_live_intervals_) {
            if (candidate.token == token) {
                live = &candidate;
                break;
            }
        }
        if (finalized_ || live == nullptr || !benchmark_e2e_origin_valid(origin)) {
            benchmark_e2e_invalid_ = true;
            return;
        }
        for (const benchmark_e2e_live_origin& attached : benchmark_e2e_live_origins_) {
            if (attached.token == token && attached.origin == origin) {
                return;
            }
        }
        if (!benchmark_e2e_live_origins_.try_push_back({
                .token = token,
                .origin = origin,
            })) {
            benchmark_e2e_mark_capacity_overflow_locked();
            return;
        }
        (void)benchmark_e2e_record_origin_locked(origin, live->kind, live->algorithm);
    }

    /** Close one physical interval at its exclusive endpoint. */
    void end_benchmark_e2e_interval(const std::uint64_t token,
                                    const std::int64_t signed_finished_ns) noexcept {
        std::scoped_lock lock{mutex_};
        if (finalized_ || token == 0u || signed_finished_ns <= 0) {
            benchmark_e2e_invalid_ = true;
            return;
        }
        std::size_t live_index = benchmark_e2e_live_intervals_.size();
        for (std::size_t index = 0; index < benchmark_e2e_live_intervals_.size(); ++index) {
            if (benchmark_e2e_live_intervals_[index].token == token) {
                live_index = index;
                break;
            }
        }
        if (live_index == benchmark_e2e_live_intervals_.size()) {
            benchmark_e2e_invalid_ = true;
            return;
        }
        const std::uint64_t finished_ns = static_cast<std::uint64_t>(signed_finished_ns);
        const benchmark_e2e_live_interval& live = benchmark_e2e_live_intervals_[live_index];
        if (finished_ns < live.started_ns) {
            benchmark_e2e_invalid_ = true;
            return;
        }

        if (!benchmark_e2e_completed_intervals_.try_push_back({
                .started_ns = live.started_ns,
                .finished_ns = finished_ns,
            })) {
            benchmark_e2e_mark_capacity_overflow_locked();
            return;
        }

        benchmark_e2e_live_intervals_.erase_at(live_index);
        for (std::size_t index = benchmark_e2e_live_origins_.size(); index != 0u; --index) {
            if (benchmark_e2e_live_origins_[index - 1u].token == token) {
                benchmark_e2e_live_origins_.erase_at(index - 1u);
            }
        }
    }

    /** Fail the interval evidence independently of the broad downstream diagnostic. */
    void invalidate_benchmark_e2e_intervals() noexcept {
        std::scoped_lock lock{mutex_};
        benchmark_e2e_invalid_ = true;
    }
#endif

    [[nodiscard]] downstream_active_wall_receipt finalize() noexcept {
        std::scoped_lock lock{mutex_};
        if (finalized_) {
            invalid_ = true;
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
            benchmark_e2e_invalid_ = true;
            downstream_active_wall_receipt duplicate{
                .elapsed_ns = accumulated_ns_,
                .available = false,
            };
            duplicate.benchmark_e2e_intervals = {
                .interval_union =
                    {
                        .union_ns = 0,
                        .interval_count = benchmark_e2e_interval_count_,
                        .cigar_interval_count = benchmark_e2e_cigar_interval_count_,
                        .mapq_interval_count = benchmark_e2e_mapq_interval_count_,
                        .available = false,
                        .invalid = true,
                        .capacity_overflow = benchmark_e2e_capacity_overflow_,
                    },
                .origins = {},
            };
            return duplicate;
#else
            return {.elapsed_ns = accumulated_ns_, .available = false};
#endif
        }
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
        // ExecutorFinish precedes ExecutorEnd, so the weak owner must still lock
        // here without the tracker extending its lifetime.
        validate_benchmark_e2e_statement_plan_locked();
#endif
        finalized_ = true;
        downstream_active_wall_receipt receipt{
            .elapsed_ns = accumulated_ns_,
            .available = observed_interval_ && active_count_ == 0u && !invalid_,
        };
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
        const bool counts_coherent =
            benchmark_e2e_cigar_interval_count_ <= benchmark_e2e_interval_count_ &&
            benchmark_e2e_mapq_interval_count_ <= benchmark_e2e_interval_count_ &&
            benchmark_e2e_cigar_interval_count_ ==
                benchmark_e2e_interval_count_ - benchmark_e2e_mapq_interval_count_;
        benchmark_e2e_completed_intervals_.sort(
            [](const benchmark_e2e_completed_interval& lhs,
               const benchmark_e2e_completed_interval& rhs) noexcept {
                if (lhs.started_ns != rhs.started_ns) {
                    return lhs.started_ns < rhs.started_ns;
                }
                return lhs.finished_ns < rhs.finished_ns;
            });
        std::uint64_t benchmark_e2e_union_ns = 0;
        bool benchmark_e2e_union_valid = true;
        if (!benchmark_e2e_completed_intervals_.empty()) {
            std::uint64_t merged_started_ns = benchmark_e2e_completed_intervals_.front().started_ns;
            std::uint64_t merged_finished_ns =
                benchmark_e2e_completed_intervals_.front().finished_ns;
            if (merged_finished_ns < merged_started_ns) {
                benchmark_e2e_union_valid = false;
            }
            const auto commit_merged_interval =
                [&benchmark_e2e_union_ns, &benchmark_e2e_union_valid](
                    const std::uint64_t started_ns, const std::uint64_t finished_ns) noexcept {
                    if (!benchmark_e2e_union_valid || finished_ns < started_ns) {
                        benchmark_e2e_union_valid = false;
                        return;
                    }
                    const std::uint64_t elapsed_ns = finished_ns - started_ns;
                    if (elapsed_ns >
                        (std::numeric_limits<std::uint64_t>::max)() - benchmark_e2e_union_ns) {
                        benchmark_e2e_union_valid = false;
                        return;
                    }
                    benchmark_e2e_union_ns += elapsed_ns;
                };
            for (std::size_t index = 1; index < benchmark_e2e_completed_intervals_.size();
                 ++index) {
                const benchmark_e2e_completed_interval& interval =
                    benchmark_e2e_completed_intervals_[index];
                if (interval.finished_ns < interval.started_ns) {
                    benchmark_e2e_union_valid = false;
                    break;
                }
                if (interval.started_ns <= merged_finished_ns) {
                    merged_finished_ns = merged_finished_ns < interval.finished_ns
                                             ? interval.finished_ns
                                             : merged_finished_ns;
                    continue;
                }
                commit_merged_interval(merged_started_ns, merged_finished_ns);
                merged_started_ns = interval.started_ns;
                merged_finished_ns = interval.finished_ns;
            }
            commit_merged_interval(merged_started_ns, merged_finished_ns);
        }
        if (!benchmark_e2e_union_valid) {
            benchmark_e2e_invalid_ = true;
            benchmark_e2e_union_ns = 0;
        }
        benchmark_e2e_origins_.sort([](const benchmark_e2e_interval_origin_summary& lhs,
                                       const benchmark_e2e_interval_origin_summary& rhs) noexcept {
            if (lhs.origin.logical_target_row != rhs.origin.logical_target_row) {
                return lhs.origin.logical_target_row < rhs.origin.logical_target_row;
            }
            if (lhs.origin.query_ordinal != rhs.origin.query_ordinal) {
                return lhs.origin.query_ordinal < rhs.origin.query_ordinal;
            }
            if (lhs.kind != rhs.kind) {
                return static_cast<std::uint8_t>(lhs.kind) < static_cast<std::uint8_t>(rhs.kind);
            }
            return static_cast<std::uint8_t>(lhs.algorithm) <
                   static_cast<std::uint8_t>(rhs.algorithm);
        });
        receipt.benchmark_e2e_intervals = {
            .interval_union =
                {
                    .union_ns = benchmark_e2e_union_ns,
                    .interval_count = benchmark_e2e_interval_count_,
                    .cigar_interval_count = benchmark_e2e_cigar_interval_count_,
                    .mapq_interval_count = benchmark_e2e_mapq_interval_count_,
                    .available = benchmark_e2e_interval_count_ != 0u &&
                                 benchmark_e2e_live_intervals_.empty() &&
                                 benchmark_e2e_live_origins_.empty() &&
                                 benchmark_e2e_completed_intervals_.size() ==
                                     benchmark_e2e_interval_count_ &&
                                 counts_coherent && !benchmark_e2e_invalid_,
                    .invalid = benchmark_e2e_invalid_ || !benchmark_e2e_live_intervals_.empty() ||
                               !benchmark_e2e_live_origins_.empty() ||
                               benchmark_e2e_completed_intervals_.size() !=
                                   benchmark_e2e_interval_count_ ||
                               !counts_coherent,
                    .capacity_overflow = benchmark_e2e_capacity_overflow_,
                },
            .origins = benchmark_e2e_origins_,
        };
#endif
        return receipt;
    }
};

/** Exception-safe ownership of one active interval. */
class downstream_active_wall_scope final {
  private:
    downstream_active_wall_tracker* tracker_ = nullptr;
    int uncaught_on_entry_ = 0;

  public:
    explicit downstream_active_wall_scope(downstream_active_wall_tracker* tracker) noexcept
        : tracker_(tracker), uncaught_on_entry_(std::uncaught_exceptions()) {
        if (tracker_ != nullptr)
            tracker_->enter();
    }
    downstream_active_wall_scope(const downstream_active_wall_scope&) = delete;
    downstream_active_wall_scope& operator=(const downstream_active_wall_scope&) = delete;
    ~downstream_active_wall_scope() noexcept {
        if (tracker_ == nullptr)
            return;
        if (std::uncaught_exceptions() > uncaught_on_entry_)
            tracker_->invalidate();
        tracker_->leave();
    }
};

#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
/** Exception-safe ownership of one benchmark-only physical excluded interval. */
class benchmark_e2e_interval_scope final {
  private:
    downstream_active_wall_tracker* tracker_ = nullptr;
    std::uint64_t token_ = 0;
    int uncaught_on_entry_ = 0;

  public:
    benchmark_e2e_interval_scope(downstream_active_wall_tracker* tracker,
                                 const bool enabled,
                                 const benchmark_e2e_interval_kind kind,
                                 const benchmark_e2e_interval_algorithm algorithm,
                                 const benchmark_e2e_operation_origin& origin,
                                 const bool origin_available) noexcept
        : tracker_(enabled ? tracker : nullptr), uncaught_on_entry_(std::uncaught_exceptions()) {
        if (tracker_ == nullptr) {
            return;
        }
        if (!origin_available) {
            tracker_->invalidate_benchmark_e2e_intervals();
            tracker_ = nullptr;
            return;
        }
        const std::int64_t started_ns = bioseq::instrumentation::clock::now_ns();
        token_ = tracker_->begin_benchmark_e2e_interval(kind, algorithm, origin, started_ns);
    }
    benchmark_e2e_interval_scope(const benchmark_e2e_interval_scope&) = delete;
    benchmark_e2e_interval_scope& operator=(const benchmark_e2e_interval_scope&) = delete;

    void attach_origin(const benchmark_e2e_operation_origin& origin) noexcept {
        if (tracker_ != nullptr && token_ != 0u) {
            tracker_->attach_benchmark_e2e_interval_origin(token_, origin);
        }
    }

    void finish() noexcept {
        if (tracker_ == nullptr || token_ == 0u) {
            tracker_ = nullptr;
            token_ = 0u;
            return;
        }
        const std::int64_t finished_ns = bioseq::instrumentation::clock::now_ns();
        tracker_->end_benchmark_e2e_interval(token_, finished_ns);
        tracker_ = nullptr;
        token_ = 0u;
    }

    ~benchmark_e2e_interval_scope() noexcept {
        if (tracker_ == nullptr || token_ == 0u) {
            return;
        }
        const std::int64_t finished_ns = bioseq::instrumentation::clock::now_ns();
        if (std::uncaught_exceptions() > uncaught_on_entry_) {
            tracker_->invalidate_benchmark_e2e_intervals();
        }
        tracker_->end_benchmark_e2e_interval(token_, finished_ns);
    }
};
#endif

/** @brief Runtime parameters and fixed-target binding for one alignment request. */
struct minimap2_parameters {
    /** Borrowed statement owner for union-of-active downstream timing. */
    downstream_active_wall_tracker* downstream_active_wall = nullptr;
    /** Kernel table selected once for the request. */
    const ::bioseq::simd_dispatch::highway_kernel_table* highway_kernels = nullptr;
    /** Target inventory and planned target selected for the request. */
    ::bioseq::simd_dispatch::highway_dispatch_plan highway_dispatch{};
    /** Exact target that executed the request. */
    std::int64_t highway_executed_target = 0;
    /** Scheduler-owned alphabet code fixed by outer alphabet dispatch. */
    std::uint8_t alphabet_code =
        (std::numeric_limits<std::uint8_t>::max)();

    /** K-mer size in symbols. */
    std::int32_t kmer_size = 15;
    /** Minimizer window size in k-mers. */
    std::int32_t window_size = 10;
    /** Maximum reference gap accepted by chaining. */
    std::int32_t max_gap = 5000;
    /** Explicit reference gap, or -1 to use max_gap. */
    std::int32_t max_gap_ref = -1;
    /** Bandwidth shared by chaining and alignment. */
    std::int32_t bandwidth = 500;
    /** Maximum skipped predecessors before dynamic-programming termination. */
    std::int32_t max_chain_skip = 25;
    /** Maximum predecessors examined for one anchor. */
    std::int32_t max_chain_iter = 5000;
    /** Minimum seed count required for a chain. */
    std::int32_t min_seed_count = 3;
    /** Minimum accepted chain score. */
    std::int32_t min_chain_score = 40;
    /** Gap-penalty scale used by chaining. */
    float chain_gap_scale = 0.8f;
    /** Skip-penalty scale used by chaining. */
    float chain_skip_scale = 0.0f;
    /** Primary-to-secondary overlap threshold. */
    float mask_level = 0.5f;
    /** Maximum uncovered length used by overlap masking. */
    std::int32_t mask_len =
        (std::numeric_limits<std::int32_t>::max)();
    /** Secondary-to-primary score-ratio threshold. */
    float pri_ratio = 0.8f;
    /** Maximum number of secondary alignments to retain. */
    std::int32_t best_n = 5;

    /** Match score. */
    std::int32_t match_score = 2;
    /** Mismatch penalty. */
    std::int32_t mismatch_pen = 4;
    /** Short-gap open penalty. */
    std::int32_t gap_open1 = 4;
    /** Short-gap extension penalty. */
    std::int32_t gap_ext1 = 2;
    /** Long-gap open penalty. */
    std::int32_t gap_open2 = 24;
    /** Long-gap extension penalty. */
    std::int32_t gap_ext2 = 1;
    /** Ambiguous-symbol penalty. */
    std::int32_t sc_ambi = 1;
    /** Z-drop termination threshold. */
    std::int32_t zdrop = 400;
    /** Bonus for reaching sequence ends, or -1 when disabled. */
    std::int32_t end_bonus = -1;

    /** Maximum seed occurrence retained by filtering. */
    std::int32_t max_occ = 500;
    /** Mid-level seed-occurrence threshold. */
    std::int32_t mid_occ = 500;

    /**
     * Minimum gap length between consecutive seeds before invoking KSW2.
     * A zero value combines all seed-to-seed gaps into one KSW2 region.
     */
    std::int32_t min_ksw_len = 200;

    /** Enable base-level alignment and CIGAR generation. */
    bool do_cigar = true;
    /** Compute the divergence estimate from seeds. */
    bool compute_divergence = true;
    /** Compute mapping quality for output regions. */
    bool compute_mapq = true;
    /** Compute the exact unit-cost edit distance from traceback. */
    bool compute_edit_distance = false;

    /** Apply the exact strict edit predicate to aligned regions. */
    bool exact_edit_filter_enabled = false;
    /** Signed exclusive upper bound for the exact edit predicate. */
    std::int64_t exact_edit_filter_exclusive_upper_bound = 0;

    /** Enable the query-scoped SneakySnake lower-bound stage. */
    bool prefilter_enabled = false;
    /** Largest exact edit count admitted by the prefilter. */
    std::uint32_t prefilter_max_edits = 0;
    /** Backend selected for prefilter execution. */
    ::bioseq::prefilter::prefilter_backend prefilter_backend =
        ::bioseq::prefilter::prefilter_backend::automatic;

    /** Minimum block-aligner scratch block size. */
    std::size_t block_min_size = 16;
    /** Maximum block-aligner scratch block size. */
    std::size_t block_max_size = 32;

    /** Shared chaining mode selected by the runtime configuration. */
    ::bioseq::chaining::chaining_mode chaining_mode =
        ::bioseq::chaining::chaining_mode::scalar_dp;
    /** Long-join bandwidth, or zero when long joining is disabled. */
    std::int32_t bandwidth_long = 0;

    /** Chaining implementation selected once before the inner loop. */
    ::bioseq::simd::chaining_algorithm chaining_algorithm =
        ::bioseq::simd::chaining_algorithm::scalar_minimap2;
    /** Forward-chaining semantics used by SIMD chaining. */
    ::bioseq::chaining::simd_chaining_policy simd_chaining_policy =
        ::bioseq::chaining::simd_chaining_policy::exact_visited;
    /** Base-alignment implementation selected for the request. */
    ::bioseq::simd::base_alignment_algorithm base_alignment_algorithm =
        ::bioseq::simd::base_alignment_algorithm::block_aligner;
    /** Minimizer-extraction implementation selected for the request. */
    ::bioseq::simd::minimizer_algorithm minimizer_algorithm =
        ::bioseq::simd::minimizer_algorithm::scalar;

    /** Enable runtime execution accounting. */
    bool enable_profiling = false;
    /** Permit Vulkan-backed algorithms for this request. */
    bool vulkan_enabled = true;
    /** Select Vulkan seed-hit lookup. */
    bool vulkan_seed_hits_enabled = false;
    /** Cache GPU-resident minimizer indexes under their memory permits. */
    bool vulkan_index_cache_enabled = true;
    /** Select the resident two-submit Vulkan frontend. */
    bool vulkan_fused_frontend_enabled = false;
    /** Build the target minimizer index on the GPU. */
    bool vulkan_gpu_index_build_enabled = false;
    /** Enable GPU base-alignment extension under the block aligner. */
    bool vulkan_extend_enabled = false;
    /** Ordered-sketch tile size in base pairs. */
    std::uint32_t vulkan_ordered_sketch_tile_bp = 2048;
    /** Device radix-sort digit width in bits per pass. */
    std::uint32_t vulkan_radix_digit_bits = 8;

    /**
     * @brief Build the chaining configuration for this request.
     * @return Chaining parameters with derived values prepared and the shared
     * chaining mode synchronized with chaining_algorithm.
     */
    [[nodiscard]] auto make_chaining_parameters() const -> chaining_parameters {
        chaining_parameters parameters;
        parameters.kmer_size = kmer_size;
        parameters.max_gap = max_gap;
        parameters.bandwidth = bandwidth;
        parameters.max_chain_skip = max_chain_skip;
        parameters.max_chain_iter = max_chain_iter;
        parameters.min_seed_count = min_seed_count;
        parameters.min_chain_score = min_chain_score;
        parameters.chain_gap_scale = chain_gap_scale;
        parameters.chain_skip_scale = chain_skip_scale;
        parameters.simd_policy = simd_chaining_policy;
        switch (chaining_algorithm) {
            case ::bioseq::simd::chaining_algorithm::simd_minimap2:
                parameters.chaining_mode =
                    ::bioseq::chaining::chaining_mode::simd_dp;
                break;
            case ::bioseq::simd::chaining_algorithm::range_minimum_query:
                parameters.chaining_mode =
                    ::bioseq::chaining::chaining_mode::rmq;
                break;
            case ::bioseq::simd::chaining_algorithm::
                    vulkan_tiled_dynamic_programming:
                parameters.chaining_mode =
                    ::bioseq::chaining::chaining_mode::scalar_dp;
                break;
            case ::bioseq::simd::chaining_algorithm::scalar_minimap2:
            default:
                parameters.chaining_mode =
                    ::bioseq::chaining::chaining_mode::scalar_dp;
                break;
        }
        parameters.vulkan_enabled = vulkan_enabled;
        parameters.coordinate_only =
            !do_cigar && !compute_divergence && !compute_mapq;
        parameters.prepare();
        return parameters;
    }
};

} // namespace bioseq::simd
