/**
 * @file pipeline/pipeline_stats_parts/identities.cppm
 * @brief Stable pipeline identities, stage coverage, and timing contracts.
 */
export module bioseq.pipeline.stats:identities;

import std;
import bioseq.common.checked_arithmetic;

export namespace bioseq::pipeline_stats {

/// Requested or actually executed biological compute backend.
enum class executed_backend : std::uint8_t {
    none,
    scalar_cpu,
    simd_cpu,
    vulkan_gpu,
    mixed_invalid,
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
    mixed_cpu = 5,
    /// Selected Vulkan seed/chain prefix with the required host suffix.
    hybrid_cpu_vulkan = 6,
#endif
};

/// Proven origin of the target minimizer index consumed by a job.
enum class target_index_source : std::uint8_t {
    none,
    recomputed,
    persisted,
    mixed_invalid,
};

/// Statement-stable identity for the requested and operation-observed SIMD
/// chaining policy. This deliberately has its own none/mixed sentinels rather
/// than overloading the algorithm enum: pipeline aggregation must distinguish
/// "no SIMD policy applies" from "two concrete policy identities were mixed".
enum class simd_chaining_policy_identity : std::uint8_t {
    none,
    exact_visited,
    bounded_batch_approximate,
    mixed_invalid,
};

/// Statement-stable identity for the SIMD base-alignment algorithm.  This is
/// intentionally narrower than base_alignment_algorithm: the SIMD stats
/// object only needs to distinguish an authentic Block Aligner execution from
/// absence or a mixed/forbidden alignment route.
enum class simd_alignment_algorithm_identity : std::uint8_t {
    none,
    block_aligner,
    mixed_invalid,
};

#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
/// Statement-stable requested/operation-observed minimizer identity.
enum class minimizer_algorithm_identity : std::uint8_t {
    none,
    scalar,
    simd_minimap2_reference,
    two_stack_nthash,
    vulkan_minimap_integer,
    bounded_nthash_simd,
    mixed_invalid,
};

/// Statement-stable requested/operation-observed chaining identity.
enum class chaining_algorithm_identity : std::uint8_t {
    none,
    scalar_minimap2,
    simd_minimap2,
    range_minimum_query,
    vulkan_tiled_dynamic_programming,
    mixed_invalid,
};

/// Statement-stable requested/operation-observed base-alignment identity.
/// Vulkan simple-CIGAR is a distinct completed operation, not a Block Aligner
/// execution merely because the public selector uses the block-alignment slot.
enum class base_alignment_algorithm_identity : std::uint8_t {
    none,
    ksw2,
    block_aligner,
    vulkan_simple_cigar,
    mixed_invalid,
};

/// Return the stable public spelling of a minimizer identity.
[[nodiscard]] constexpr std::string_view
minimizer_algorithm_identity_name(minimizer_algorithm_identity algorithm) noexcept {
    switch (algorithm) {
    case minimizer_algorithm_identity::none:
        return "none";
    case minimizer_algorithm_identity::scalar:
        return "scalar";
    case minimizer_algorithm_identity::simd_minimap2_reference:
        return "simd_minimap2_reference";
    case minimizer_algorithm_identity::two_stack_nthash:
        return "two_stack_nthash";
    case minimizer_algorithm_identity::vulkan_minimap_integer:
        return "vulkan_minimap_integer";
    case minimizer_algorithm_identity::bounded_nthash_simd:
        return "bounded_nthash_simd";
    case minimizer_algorithm_identity::mixed_invalid:
        return "mixed_invalid";
    }
    return "mixed_invalid";
}

/// Return the stable public spelling of a chaining identity.
[[nodiscard]] constexpr std::string_view
chaining_algorithm_identity_name(chaining_algorithm_identity algorithm) noexcept {
    switch (algorithm) {
    case chaining_algorithm_identity::none:
        return "none";
    case chaining_algorithm_identity::scalar_minimap2:
        return "scalar_minimap2";
    case chaining_algorithm_identity::simd_minimap2:
        return "simd_minimap2";
    case chaining_algorithm_identity::range_minimum_query:
        return "range_minimum_query";
    case chaining_algorithm_identity::vulkan_tiled_dynamic_programming:
        return "vulkan_tiled_dynamic_programming";
    case chaining_algorithm_identity::mixed_invalid:
        return "mixed_invalid";
    }
    return "mixed_invalid";
}

/// Return the stable public spelling of a base-alignment identity.
[[nodiscard]] constexpr std::string_view
base_alignment_algorithm_identity_name(base_alignment_algorithm_identity algorithm) noexcept {
    switch (algorithm) {
    case base_alignment_algorithm_identity::none:
        return "none";
    case base_alignment_algorithm_identity::ksw2:
        return "ksw2";
    case base_alignment_algorithm_identity::block_aligner:
        return "block_aligner";
    case base_alignment_algorithm_identity::vulkan_simple_cigar:
        return "vulkan_simple_cigar";
    case base_alignment_algorithm_identity::mixed_invalid:
        return "mixed_invalid";
    }
    return "mixed_invalid";
}

/// Merge repeated observations, retaining concrete disagreement explicitly.
[[nodiscard]] constexpr minimizer_algorithm_identity
merge_minimizer_algorithm_identity(minimizer_algorithm_identity lhs,
                                   minimizer_algorithm_identity rhs) noexcept {
    if (lhs == minimizer_algorithm_identity::none)
        return rhs;
    if (rhs == minimizer_algorithm_identity::none)
        return lhs;
    if (lhs == rhs)
        return lhs;
    return minimizer_algorithm_identity::mixed_invalid;
}

/// Merge repeated observations, retaining concrete disagreement explicitly.
[[nodiscard]] constexpr chaining_algorithm_identity
merge_chaining_algorithm_identity(chaining_algorithm_identity lhs,
                                  chaining_algorithm_identity rhs) noexcept {
    if (lhs == chaining_algorithm_identity::none)
        return rhs;
    if (rhs == chaining_algorithm_identity::none)
        return lhs;
    if (lhs == rhs)
        return lhs;
    return chaining_algorithm_identity::mixed_invalid;
}

/// Merge repeated base-alignment observations without concealing disagreement.
[[nodiscard]] constexpr base_alignment_algorithm_identity
merge_base_alignment_algorithm_identity(base_alignment_algorithm_identity lhs,
                                        base_alignment_algorithm_identity rhs) noexcept {
    if (lhs == base_alignment_algorithm_identity::none)
        return rhs;
    if (rhs == base_alignment_algorithm_identity::none)
        return lhs;
    if (lhs == rhs)
        return lhs;
    return base_alignment_algorithm_identity::mixed_invalid;
}
#endif
/// @brief Return the stable public spelling used by PostgreSQL and reporting.
///
/// Keep this on the aggregate identity rather than translating through the algorithm-local enum:
/// `none` and `mixed_invalid` are statement-level states that the local policy
/// type deliberately cannot represent.
/// @param[in] policy Aggregate SIMD chaining-policy identity.
/// @return Stable report spelling for `policy`.
[[nodiscard]] constexpr std::string_view
simd_chaining_policy_identity_name(simd_chaining_policy_identity policy) noexcept {
    switch (policy) {
    case simd_chaining_policy_identity::none:
        return "none";
    case simd_chaining_policy_identity::exact_visited:
        return "exact_visited";
    case simd_chaining_policy_identity::bounded_batch_approximate:
        return "bounded_batch_approximate";
    case simd_chaining_policy_identity::mixed_invalid:
        return "mixed_invalid";
    }
    return "mixed_invalid";
}

/// @brief Return the stable report name of a SIMD alignment identity.
/// @param[in] algorithm Aggregate SIMD alignment identity.
/// @return Stable report spelling for `algorithm`.
[[nodiscard]] constexpr std::string_view
simd_alignment_algorithm_identity_name(simd_alignment_algorithm_identity algorithm) noexcept {
    switch (algorithm) {
    case simd_alignment_algorithm_identity::none:
        return "none";
    case simd_alignment_algorithm_identity::block_aligner:
        return "block_aligner";
    case simd_alignment_algorithm_identity::mixed_invalid:
        return "mixed_invalid";
    }
    return "mixed_invalid";
}

/// Mutually exclusive engine credited with executing one pipeline stage.
enum class execution_engine : std::uint8_t {
    host_io,
    scalar_cpu,
    simd_cpu,
    vulkan_gpu,
    count,
};

/// Stable SQL/report ordinal for every attested pipeline stage.
enum class pipeline_stage : std::uint8_t {
    query_sketch = 0,
    target_sketch = 1,
    target_index_build = 2,
    persisted_index_decode = 3,
    partition_selection = 4,
    device_index_upload = 5,
    seed_lookup = 6,
    chain_score = 7,
    chain_backtrack = 8,
    region_generation = 9,
    primary_secondary_reduction = 10,
    divergence_estimation = 11,
    mapq = 12,
    target_window_plan = 13,
    target_window_io = 14,
    target_window_upload = 15,
    cigar_job_generation = 16,
    left_extension = 17,
    gap_extension = 18,
    right_extension = 19,
    count = 20,
};

/// Number of real execution engines, excluding the count sentinel.
inline constexpr std::size_t k_execution_engine_count =
    static_cast<std::size_t>(execution_engine::count);
/// Number of stable pipeline-stage ordinals, excluding the count sentinel.
inline constexpr std::size_t k_pipeline_stage_count =
    static_cast<std::size_t>(pipeline_stage::count);
/// Sentinel used when no failing target/query ordinal has been recorded.
inline constexpr std::uint64_t k_unset_job_ordinal = (std::numeric_limits<std::uint64_t>::max)();
/// Largest value representable by SQL bigint without lossy conversion.
inline constexpr std::uint64_t k_sql_bigint_value_max =
    static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());
/// Largest worker count representable by SQL integer.
inline constexpr std::uint32_t k_sql_integer_value_max =
    static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)());

/// @brief Return the stable report name for a biological backend value.
/// @param[in] backend Backend identity to format.
/// @return Stable report spelling for `backend`.
[[nodiscard]] constexpr std::string_view executed_backend_name(executed_backend backend) noexcept {
    switch (backend) {
    case executed_backend::none:
        return "none";
    case executed_backend::scalar_cpu:
        return "scalar_cpu";
    case executed_backend::simd_cpu:
        return "simd_cpu";
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
    case executed_backend::mixed_cpu:
        return "mixed_cpu";
    case executed_backend::hybrid_cpu_vulkan:
        return "hybrid_cpu_vulkan";
#endif
    case executed_backend::vulkan_gpu:
        return "vulkan_gpu";
    case executed_backend::mixed_invalid:
        return "mixed_invalid";
    }
    return "unknown";
}

/// @brief Return the stable report name for a target-index source value.
/// @param[in] source Target-index source identity to format.
/// @return Stable report spelling for `source`.
[[nodiscard]] constexpr std::string_view
target_index_source_name(target_index_source source) noexcept {
    switch (source) {
    case target_index_source::none:
        return "none";
    case target_index_source::recomputed:
        return "recomputed";
    case target_index_source::persisted:
        return "persisted";
    case target_index_source::mixed_invalid:
        return "mixed_invalid";
    }
    return "unknown";
}

/// @brief Return the stable report name for an execution engine value.
/// @param[in] engine Execution-engine identity to format.
/// @return Stable report spelling for `engine`.
[[nodiscard]] constexpr std::string_view execution_engine_name(execution_engine engine) noexcept {
    switch (engine) {
    case execution_engine::host_io:
        return "host_io";
    case execution_engine::scalar_cpu:
        return "scalar_cpu";
    case execution_engine::simd_cpu:
        return "simd_cpu";
    case execution_engine::vulkan_gpu:
        return "vulkan_gpu";
    case execution_engine::count:
        return "count";
    }
    return "unknown";
}

/// @brief Return the stable report/SRF name for a pipeline-stage ordinal.
/// @param[in] stage Pipeline-stage identity to format.
/// @return Stable report spelling for `stage`.
[[nodiscard]] constexpr std::string_view pipeline_stage_name(pipeline_stage stage) noexcept {
    switch (stage) {
    case pipeline_stage::query_sketch:
        return "query_sketch";
    case pipeline_stage::target_sketch:
        return "target_sketch";
    case pipeline_stage::target_index_build:
        return "target_index_build";
    case pipeline_stage::persisted_index_decode:
        return "persisted_index_decode";
    case pipeline_stage::partition_selection:
        return "partition_selection";
    case pipeline_stage::device_index_upload:
        return "device_index_upload";
    case pipeline_stage::seed_lookup:
        return "seed_lookup";
    case pipeline_stage::chain_score:
        return "chain_score";
    case pipeline_stage::chain_backtrack:
        return "chain_backtrack";
    case pipeline_stage::region_generation:
        return "region_generation";
    case pipeline_stage::primary_secondary_reduction:
        return "primary_secondary_reduction";
    case pipeline_stage::divergence_estimation:
        return "divergence_estimation";
    case pipeline_stage::mapq:
        return "mapq";
    case pipeline_stage::target_window_plan:
        return "target_window_plan";
    case pipeline_stage::target_window_io:
        return "target_window_io";
    case pipeline_stage::target_window_upload:
        return "target_window_upload";
    case pipeline_stage::cigar_job_generation:
        return "cigar_job_generation";
    case pipeline_stage::left_extension:
        return "left_extension";
    case pipeline_stage::gap_extension:
        return "gap_extension";
    case pipeline_stage::right_extension:
        return "right_extension";
    case pipeline_stage::count:
        return "count";
    }
    return "unknown";
}

/// @brief Convert a validated engine enum to its coverage-array index.
/// @param[in] engine Validated execution-engine identity.
/// @return Array index corresponding to `engine`.
[[nodiscard]] constexpr std::size_t engine_index(execution_engine engine) noexcept {
    return static_cast<std::size_t>(engine);
}

/// @brief Convert a validated stage enum to its coverage-array index.
/// @param[in] stage Validated pipeline-stage identity.
/// @return Array index corresponding to `stage`.
[[nodiscard]] constexpr std::size_t stage_index(pipeline_stage stage) noexcept {
    return static_cast<std::size_t>(stage);
}

/// @brief Merge a SQL-bound count/time/byte value with shared saturating arithmetic.
///
/// PostgreSQL bigint is signed, so values clamp at
/// INT64_MAX. An exact sum of INT64_MAX is representable and is not saturation.
/// @param[in,out] destination SQL-bound accumulator to update.
/// @param[in] increment Value to add before clamping.
/// @return `true` when the destination had to be clamped.
[[nodiscard]] inline bool merge_counter(std::uint64_t& destination,
                                        std::uint64_t increment) noexcept {
    if (destination > k_sql_bigint_value_max) {
        destination = k_sql_bigint_value_max;
        return true;
    }
    if (increment > k_sql_bigint_value_max - destination) {
        destination = k_sql_bigint_value_max;
        return true;
    }
    destination = bioseq::common::saturating_add(destination, increment);
    return false;
}

/// Target-independent projection of the operation-local Block Aligner stats.
/// Every field is copied at the dispatch boundary from
/// block_aligner_execution_stats. Stage coverage and generic SIMD counters are
/// never used to manufacture these statistics.
struct block_aligner_stage_stats {
    bool is_available = false;
    bool is_accepted = false;
    std::uint64_t executed_calls = 0;
    std::uint64_t nonempty_calls = 0;
    std::uint64_t nonempty_calls_with_simd = 0;
    std::uint64_t simd_dp_vector_batches = 0;
    std::uint64_t simd_dp_cells = 0;
    std::uint64_t forbidden_ksw2_calls = 0;
    std::uint64_t forbidden_scalar_biological_fallbacks = 0;
    bool is_saturated = false;

    [[nodiscard]] constexpr bool
    operator==(const block_aligner_stage_stats&) const noexcept = default;

    /// @brief Merge independently captured operation statistics.
    ///
    /// Every available operand must be accepted and SQL-bound counter overflow revokes it.
    /// @param[in] rhs Operation-local statistics to merge.
    /// @return `true` when either operand or the merged result is saturated.
    [[nodiscard]] bool merge(const block_aligner_stage_stats& rhs) noexcept {
        if (!rhs.is_available) {
            const bool malformed_absence =
                rhs.is_accepted || rhs.executed_calls != 0 || rhs.nonempty_calls != 0 ||
                rhs.nonempty_calls_with_simd != 0 || rhs.simd_dp_vector_batches != 0 ||
                rhs.simd_dp_cells != 0 || rhs.forbidden_ksw2_calls != 0 ||
                rhs.forbidden_scalar_biological_fallbacks != 0 || rhs.is_saturated;
            if (malformed_absence) {
                is_saturated = true;
                is_accepted = false;
            }
            return is_saturated;
        }

        if (!is_available) {
            is_available = true;
            is_accepted = rhs.is_accepted;
        } else {
            is_accepted = is_accepted && rhs.is_accepted;
        }
        bool overflow = is_saturated || rhs.is_saturated;
        overflow = merge_counter(executed_calls, rhs.executed_calls) || overflow;
        overflow = merge_counter(nonempty_calls, rhs.nonempty_calls) || overflow;
        overflow =
            merge_counter(nonempty_calls_with_simd, rhs.nonempty_calls_with_simd) || overflow;
        overflow = merge_counter(simd_dp_vector_batches, rhs.simd_dp_vector_batches) || overflow;
        overflow = merge_counter(simd_dp_cells, rhs.simd_dp_cells) || overflow;
        overflow = merge_counter(forbidden_ksw2_calls, rhs.forbidden_ksw2_calls) || overflow;
        overflow = merge_counter(forbidden_scalar_biological_fallbacks,
                                 rhs.forbidden_scalar_biological_fallbacks) ||
                   overflow;
        is_saturated = overflow;
        if (is_saturated) {
            is_accepted = false;
        }
        return is_saturated;
    }
};

/// @brief Count distinct logical target-row identities in-place.
///
/// Physical payload coalescing is deliberately irrelevant: two relation rows with identical
/// sequence bytes remain two logical targets, while repeated observations of
/// the same row identity collapse to one target outcome.
/// @param[in,out] logical_rows Row identities reordered in-place for deduplication.
/// @return Number of distinct row identities after in-place ordering.
[[nodiscard]] inline std::uint64_t
count_unique_logical_target_rows(std::span<std::uint64_t> logical_rows) {
    std::sort(logical_rows.begin(), logical_rows.end());
    const auto unique_end = std::unique(logical_rows.begin(), logical_rows.end());
    return static_cast<std::uint64_t>(unique_end - logical_rows.begin());
}

/// Binary per-job coverage or its saturating statement aggregate.
struct stage_coverage {
    std::uint64_t eligible_jobs = 0;
    std::array<std::uint64_t, k_execution_engine_count> executed_jobs{};
    std::uint64_t fallback_jobs = 0;

    [[nodiscard]] constexpr bool operator==(const stage_coverage&) const noexcept = default;

    /// @brief Saturating merge of one stage coverage value.
    /// @param[in] rhs Stage coverage to merge.
    /// @return `true` when at least one merged counter saturated.
    [[nodiscard]] bool merge(const stage_coverage& rhs) noexcept {
        bool is_saturated = merge_counter(eligible_jobs, rhs.eligible_jobs);
        for (std::size_t i = 0; i < executed_jobs.size(); ++i) {
            is_saturated = merge_counter(executed_jobs[i], rhs.executed_jobs[i]) || is_saturated;
        }
        is_saturated = merge_counter(fallback_jobs, rhs.fallback_jobs) || is_saturated;
        return is_saturated;
    }
};

/// Coverage for every stable pipeline-stage ordinal.
struct pipeline_job_stage_coverage {
    std::array<stage_coverage, k_pipeline_stage_count> coverage{};

    [[nodiscard]] constexpr bool
    operator==(const pipeline_job_stage_coverage&) const noexcept = default;

    /// @brief Merge all stage coverages and report whether any count is saturated.
    /// @param[in] rhs Per-stage coverage to merge.
    /// @return `true` when at least one stage counter saturated.
    [[nodiscard]] bool merge(const pipeline_job_stage_coverage& rhs) noexcept {
        bool is_saturated = false;
        for (std::size_t i = 0; i < coverage.size(); ++i) {
            is_saturated = coverage[i].merge(rhs.coverage[i]) || is_saturated;
        }
        return is_saturated;
    }
};

/// Host and device time attributed to one pipeline stage.
struct pipeline_stage_timing {
    std::uint64_t host_wall_ns = 0;
    std::uint64_t gpu_device_ns = 0;

    [[nodiscard]] constexpr bool operator==(const pipeline_stage_timing&) const noexcept = default;
};

/// @brief Merge a lossless u64 identity that may be published exactly once.
/// @param[in,out] destination Retained identity value.
/// @param[in,out] destination_available Presence bit for `destination`.
/// @param[in] incoming Incoming identity value.
/// @param[in] incoming_available Presence bit for `incoming`.
/// @param[in,out] has_identity_conflict Retained fail-closed conflict flag.
inline void merge_report_once_identity(std::uint64_t& destination,
                                       bool& destination_available,
                                       std::uint64_t incoming,
                                       bool incoming_available,
                                       bool& has_identity_conflict) noexcept {
    if ((!destination_available && destination != 0) || (!incoming_available && incoming != 0)) {
        has_identity_conflict = true;
    }
    if (!incoming_available)
        return;
    if (!destination_available) {
        destination = incoming;
        destination_available = true;
        return;
    }
    has_identity_conflict = true;
}

} // namespace bioseq::pipeline_stats
