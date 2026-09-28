/**
 * @file pipeline/pipeline_stats_parts/environment_attestation.cppm
 * @brief Record the execution  environment and check each job's stage, statistics.
 */
export module bioseq.pipeline.stats:environment_attestation;

import std;
import :identities;
import bioseq.pipeline.runtime_contract;

export namespace bioseq::pipeline_stats {

/// Optional true or false observation, with a separate conflict value.
enum class optional_observation_bool : std::uint8_t {
    not_applicable,
    false_value,
    true_value,
    mixed_invalid,
};

/// Pipeline-cache policy recorded by the timed GPU runtime.
enum class pipeline_cache_policy : std::uint8_t {
    not_applicable,
    immutable_preseed_all_hit,
    isolated_miss_diagnostic,
    native_vulkan,
    mixed_invalid,
};

/// Return the observation name used in SQL and reports.
[[nodiscard]] constexpr std::string_view
optional_observation_name(optional_observation_bool value) noexcept {
    switch (value) {
    case optional_observation_bool::not_applicable:
        return "not_applicable";
    case optional_observation_bool::false_value:
        return "false";
    case optional_observation_bool::true_value:
        return "true";
    case optional_observation_bool::mixed_invalid:
        return "mixed_invalid";
    }
    return "unknown";
}

/// Return the pipeline-cache policy name usec in SQL and reports.
[[nodiscard]] constexpr std::string_view
pipeline_cache_policy_name(pipeline_cache_policy policy) noexcept {
    switch (policy) {
    case pipeline_cache_policy::not_applicable:
        return "not_applicable";
    case pipeline_cache_policy::immutable_preseed_all_hit:
        return "immutable_preseed_all_hit";
    case pipeline_cache_policy::isolated_miss_diagnostic:
        return "isolated_miss_diagnostic";
    case pipeline_cache_policy::native_vulkan:
        return "native_vulkan";
    case pipeline_cache_policy::mixed_invalid:
        return "mixed_invalid";
    }
    return "unknown";
}

/// Check that every array element equals p_value_type{}.
template <typename p_value_type, std::size_t p_size>
[[nodiscard]] constexpr bool is_all_zero(const std::array<p_value_type, p_size>& value) noexcept {
    for (const p_value_type element : value) {
        if (element != p_value_type{})
            return false;
    }
    return true;
}

/// Worker count, Highway targets and GPU/cache settings recorded for execution.
struct execution_environment_snapshot {
    std::uint32_t effective_cpu_workers = 0;
    std::int64_t highway_compiled_targets = 0;
    std::int64_t highway_cpu_supported_targets = 0;
    std::int64_t highway_runnable_targets = 0;
    std::int64_t highway_executed_target = 0;
    bool is_highway_executed_target_mixed = false;
    bool is_gpu_environment_available = false;
    std::array<std::uint8_t, 16> gpu_device_uuid{};
    std::array<char, 128> gpu_driver_identity{};
    pipeline_cache_policy gpu_pipeline_cache_policy = pipeline_cache_policy::not_applicable;
    std::array<std::uint8_t, 32> gpu_pipeline_cache_seed_sha256{};
    std::uint64_t gpu_pipeline_cache_seed_size = 0;
    std::uint64_t gpu_pipeline_cache_manifest_checksum = 0;
    optional_observation_bool gpu_pipeline_cache_hit = optional_observation_bool::not_applicable;
    bool is_mixed_invalid = false;

    /// Check that every environment field is still unset
    [[nodiscard]] static constexpr bool
    is_unset(const execution_environment_snapshot& value) noexcept {
        return value.effective_cpu_workers == 0 && value.highway_compiled_targets == 0 &&
               value.highway_cpu_supported_targets == 0 && value.highway_runnable_targets == 0 &&
               value.highway_executed_target == 0 && !value.is_highway_executed_target_mixed &&
               !value.is_gpu_environment_available && is_all_zero(value.gpu_device_uuid) &&
               is_all_zero(value.gpu_driver_identity) &&
               value.gpu_pipeline_cache_policy == pipeline_cache_policy::not_applicable &&
               is_all_zero(value.gpu_pipeline_cache_seed_sha256) &&
               value.gpu_pipeline_cache_seed_size == 0 &&
               value.gpu_pipeline_cache_manifest_checksum == 0 &&
               value.gpu_pipeline_cache_hit == optional_observation_bool::not_applicable &&
               !value.is_mixed_invalid;
    }

    /// Compare worker, GPU and Highway  plan fields. Executed-target observations
    /// and conflict flags are handled separately.
    [[nodiscard]] static constexpr bool
    is_payload_equal(const execution_environment_snapshot& lhs,
                     const execution_environment_snapshot& rhs) noexcept {
        return non_highway_is_payload_equal(lhs, rhs) &&
               lhs.highway_compiled_targets == rhs.highway_compiled_targets &&
               lhs.highway_cpu_supported_targets == rhs.highway_cpu_supported_targets &&
               lhs.highway_runnable_targets == rhs.highway_runnable_targets;
    }

    /// Compare environment settings while allowing a missing Highway mask.
    /// Merging can fill a zero mask from another record. Final validation still
    /// requires a complete plan and one runnable executed target.
    [[nodiscard]] static constexpr bool
    has_same_payload_identity(const execution_environment_snapshot& lhs,
                              const execution_environment_snapshot& rhs) noexcept {
        const auto mask_compatible = [](const std::int64_t left,
                                        const std::int64_t right) noexcept {
            return left == 0 || right == 0 || left == right;
        };
        return non_highway_is_payload_equal(lhs, rhs) &&
               mask_compatible(lhs.highway_compiled_targets, rhs.highway_compiled_targets) &&
               mask_compatible(lhs.highway_cpu_supported_targets,
                               rhs.highway_cpu_supported_targets) &&
               mask_compatible(lhs.highway_runnable_targets, rhs.highway_runnable_targets);
    }

    /// Check that no Highway plan, executed target or target conflict was recorded.
    [[nodiscard]] static constexpr bool
    has_no_highway_fields(const execution_environment_snapshot& value) noexcept {
        return value.highway_compiled_targets == 0 && value.highway_cpu_supported_targets == 0 &&
               value.highway_runnable_targets == 0 && value.highway_executed_target == 0 &&
               !value.is_highway_executed_target_mixed;
    }

    /// Accept an absent Highway record or a complete valid one. A  complete record
    /// needs runnable = compiled & CPU-supported and one executed target from
    /// that set. Mixed targets fail validation.
    [[nodiscard]] static constexpr bool
    has_valid_highway_fields(const execution_environment_snapshot& value) noexcept {
        if (has_no_highway_fields(value))
            return true;
        if (value.is_highway_executed_target_mixed || value.highway_compiled_targets == 0 ||
            value.highway_cpu_supported_targets == 0 || value.highway_runnable_targets == 0 ||
            value.highway_runnable_targets !=
                (value.highway_compiled_targets & value.highway_cpu_supported_targets)) {
            return false;
        }
        const auto executed = static_cast<std::uint64_t>(value.highway_executed_target);
        const auto runnable = static_cast<std::uint64_t>(value.highway_runnable_targets);
        return std::has_single_bit(executed) && (executed & runnable) == executed;
    }

  private:
    /// Compare the worker count and GPU/cache fields.
    [[nodiscard]] static constexpr bool
    non_highway_is_payload_equal(const execution_environment_snapshot& lhs,
                                 const execution_environment_snapshot& rhs) noexcept {
        return lhs.effective_cpu_workers == rhs.effective_cpu_workers &&
               lhs.is_gpu_environment_available == rhs.is_gpu_environment_available &&
               lhs.gpu_device_uuid == rhs.gpu_device_uuid &&
               lhs.gpu_driver_identity == rhs.gpu_driver_identity &&
               lhs.gpu_pipeline_cache_policy == rhs.gpu_pipeline_cache_policy &&
               lhs.gpu_pipeline_cache_seed_sha256 == rhs.gpu_pipeline_cache_seed_sha256 &&
               lhs.gpu_pipeline_cache_seed_size == rhs.gpu_pipeline_cache_seed_size &&
               lhs.gpu_pipeline_cache_manifest_checksum ==
                   rhs.gpu_pipeline_cache_manifest_checksum &&
               lhs.gpu_pipeline_cache_hit == rhs.gpu_pipeline_cache_hit;
    }

  public:
    /// Merge environment observations and flag disagreement. Missing Highway
    /// masks can be filled later. Different executed targets are recorded as mixed.
    void merge(const execution_environment_snapshot& rhs) noexcept {
        if (is_unset(*this)) {
            *this = rhs;
            if (rhs.effective_cpu_workers == 0 || rhs.is_mixed_invalid) {
                is_mixed_invalid = true;
            }
            return;
        }
        if (is_unset(rhs) || effective_cpu_workers == 0 || rhs.effective_cpu_workers == 0 ||
            is_mixed_invalid || rhs.is_mixed_invalid || !has_same_payload_identity(*this, rhs)) {
            is_mixed_invalid = true;
            return;
        }

        const auto merge_highway_mask = [this](std::int64_t& destination,
                                               const std::int64_t observed) noexcept {
            if (observed == 0)
                return;
            if (destination == 0) {
                destination = observed;
            } else if (destination != observed) {
                is_mixed_invalid = true;
            }
        };
        merge_highway_mask(highway_compiled_targets, rhs.highway_compiled_targets);
        merge_highway_mask(highway_cpu_supported_targets, rhs.highway_cpu_supported_targets);
        merge_highway_mask(highway_runnable_targets, rhs.highway_runnable_targets);

        if (is_highway_executed_target_mixed || rhs.is_highway_executed_target_mixed) {
            is_highway_executed_target_mixed = true;
            highway_executed_target = 0;
        } else if (highway_executed_target == 0) {
            highway_executed_target = rhs.highway_executed_target;
        } else if (rhs.highway_executed_target != 0 &&
                   highway_executed_target != rhs.highway_executed_target) {
            is_highway_executed_target_mixed = true;
            highway_executed_target = 0;
        }
    }

    /// Record the Highway plan and the target returned by dispatch.
    /// Invalid or changed plans mark the environment invalid. Different executed
    /// targets wlear the single-target value and set the  mixed-target flag.
    void record_highway_target(const ::bioseq::simd_dispatch::highway_dispatch_plan& plan,
                               const std::int64_t executed_target) noexcept {
        if (highway_compiled_targets == 0 && highway_cpu_supported_targets == 0 &&
            highway_runnable_targets == 0) {
            highway_compiled_targets = plan.compiled_targets;
            highway_cpu_supported_targets = plan.cpu_supported_targets;
            highway_runnable_targets = plan.runnable_targets;
        } else if (highway_compiled_targets != plan.compiled_targets ||
                   highway_cpu_supported_targets != plan.cpu_supported_targets ||
                   highway_runnable_targets != plan.runnable_targets) {
            is_mixed_invalid = true;
        }

        const auto executed = static_cast<std::uint64_t>(executed_target);
        const auto runnable = static_cast<std::uint64_t>(plan.runnable_targets);
        if (plan.compiled_targets == 0 || plan.cpu_supported_targets == 0 ||
            plan.runnable_targets == 0 ||
            plan.runnable_targets != (plan.compiled_targets & plan.cpu_supported_targets) ||
            plan.planned_target != executed_target || !std::has_single_bit(executed) ||
            (executed & runnable) != executed) {
            is_mixed_invalid = true;
            return;
        }
        if (is_highway_executed_target_mixed) {
            return;
        }
        if (highway_executed_target == 0) {
            highway_executed_target = executed_target;
        } else if (highway_executed_target != executed_target) {
            highway_executed_target = 0;
            is_highway_executed_target_mixed = true;
        }
    }
};

/// Requested settings and recorded execution for one target-query job.
struct pipeline_job_execution_stats {
    executed_backend requested_backend = executed_backend::none;
    executed_backend actual_backend = executed_backend::none;
    simd_chaining_policy_identity requested_simd_chaining_policy =
        simd_chaining_policy_identity::none;
    simd_chaining_policy_identity observed_simd_chaining_policy =
        simd_chaining_policy_identity::none;
    simd_alignment_algorithm_identity requested_simd_alignment_algorithm =
        simd_alignment_algorithm_identity::none;
    simd_alignment_algorithm_identity observed_simd_alignment_algorithm =
        simd_alignment_algorithm_identity::none;
    target_index_source requested_target_index_source = target_index_source::none;
    target_index_source actual_target_index_source = target_index_source::none;
    std::uint64_t persisted_generation_id = 0;
    std::uint64_t persisted_source_identity = 0;
    std::uint64_t logical_target_row = 0;
    std::uint64_t query_ordinal = 0;
    execution_environment_snapshot environment{};
    pipeline_job_stage_coverage stages{};
};

/// Check whether the value names a supported execution backend.
[[nodiscard]] constexpr bool is_concrete_backend(executed_backend backend) noexcept {
    return backend == executed_backend::scalar_cpu || backend == executed_backend::simd_cpu ||
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
           backend == executed_backend::mixed_cpu ||
           backend == executed_backend::hybrid_cpu_vulkan ||
#endif
           backend == executed_backend::vulkan_gpu;
}

/// Check whether the index source, is recomputed or persisted.
[[nodiscard]] constexpr bool is_concrete_target_index_source(target_index_source source) noexcept {
    return source == target_index_source::recomputed || source == target_index_source::persisted;
}

/// Combine backend observations, ignoring none and returning mixed_invalid
/// when recorded backends disagree.
[[nodiscard]] constexpr executed_backend merge_backend_identity(executed_backend lhs,
                                                                executed_backend rhs) noexcept {
    if (lhs == executed_backend::none)
        return rhs;
    if (rhs == executed_backend::none)
        return lhs;
    if (lhs == rhs)
        return lhs;
    return executed_backend::mixed_invalid;
}

/// Combine index-source observations, returning mixed_invalid on disagreement
[[nodiscard]] constexpr target_index_source
merge_target_index_identity(target_index_source lhs, target_index_source rhs) noexcept {
    if (lhs == target_index_source::none)
        return rhs;
    if (rhs == target_index_source::none)
        return lhs;
    if (lhs == rhs)
        return lhs;
    return target_index_source::mixed_invalid;
}

/// Check whether the value names an executable SIMD chaining policy.
[[nodiscard]] constexpr bool
is_concrete_simd_chaining_policy_identity(simd_chaining_policy_identity policy) noexcept {
    return policy == simd_chaining_policy_identity::exact_visited ||
           policy == simd_chaining_policy_identity::bounded_batch_approximate;
}

/// Combine SIMD chaining policies. Missing observations are ignored and
/// different recorded policies give mixed_invalid.
[[nodiscard]] constexpr simd_chaining_policy_identity
merge_simd_chaining_policy_identity(simd_chaining_policy_identity lhs,
                                    simd_chaining_policy_identity rhs) noexcept {
    if (lhs == simd_chaining_policy_identity::none)
        return rhs;
    if (rhs == simd_chaining_policy_identity::none)
        return lhs;
    if (lhs == rhs)
        return lhs;
    return simd_chaining_policy_identity::mixed_invalid;
}

/// Check whether the SIMD alignment identity is Block Aligner.
[[nodiscard]] constexpr bool is_concrete_simd_alignment_algorithm_identity(
    simd_alignment_algorithm_identity algorithm) noexcept {
    return algorithm == simd_alignment_algorithm_identity::block_aligner;
}

/// Combine SIMD alignment observations, returning mixed_invalid on disagreement.
[[nodiscard]] constexpr simd_alignment_algorithm_identity
merge_simd_alignment_algorithm_identity(simd_alignment_algorithm_identity lhs,
                                        simd_alignment_algorithm_identity rhs) noexcept {
    if (lhs == simd_alignment_algorithm_identity::none)
        return rhs;
    if (rhs == simd_alignment_algorithm_identity::none)
        return lhs;
    if (lhs == rhs)
        return lhs;
    return simd_alignment_algorithm_identity::mixed_invalid;
}

/// Check the requested and recorded SIMD alignment  for this backend.
/// SIMD requires Block Aligner. Mixed routes allow matching none or Block
/// Aligner values. Other backends require both values to be none.
[[nodiscard]] constexpr bool is_simd_alignment_algorithm_identity_valid_for_backend(
    executed_backend backend,
    simd_alignment_algorithm_identity requested,
    simd_alignment_algorithm_identity observed) noexcept {
    if (backend == executed_backend::simd_cpu) {
        return is_concrete_simd_alignment_algorithm_identity(requested) && requested == observed;
    }
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
    if (backend == executed_backend::mixed_cpu ||
        backend == executed_backend::hybrid_cpu_vulkan) {
        return requested != simd_alignment_algorithm_identity::mixed_invalid &&
               requested == observed;
    }
#endif
    return requested == simd_alignment_algorithm_identity::none &&
           observed == simd_alignment_algorithm_identity::none;
}

/// Check the requested and recorded SIMD chaining policy. SIMD requires a
/// matching executable policy. Mixed routes also allow matching none values.
/// Other backends require both values to be none.
[[nodiscard]] constexpr bool is_simd_chaining_policy_identity_valid_for_backend(
    executed_backend backend,
    simd_chaining_policy_identity requested,
    simd_chaining_policy_identity observed) noexcept {
    if (backend == executed_backend::simd_cpu) {
        return is_concrete_simd_chaining_policy_identity(requested) && requested == observed;
    }
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
    if (backend == executed_backend::mixed_cpu ||
        backend == executed_backend::hybrid_cpu_vulkan) {
        return requested != simd_chaining_policy_identity::mixed_invalid && requested == observed;
    }
#endif
    return requested == simd_chaining_policy_identity::none &&
           observed == simd_chaining_policy_identity::none;
}

/// Require a nonempty UTF-8 driver name terminated and padded with zero bytes.
[[nodiscard]] constexpr bool
is_canonical_nonempty_driver_identity(const std::array<char, 128>& identity) noexcept {
    const auto byte_at = [&identity](std::size_t index) noexcept {
        return static_cast<std::uint8_t>(identity[index]);
    };
    const auto continuation = [&byte_at](std::size_t index) noexcept {
        const std::uint8_t byte = byte_at(index);
        return byte >= 0x80u && byte <= 0xbfu;
    };
    if (byte_at(0) == 0)
        return false;
    std::size_t index = 0;
    while (index < identity.size()) {
        const std::uint8_t lead = byte_at(index);
        if (lead == 0) {
            for (; index < identity.size(); ++index) {
                if (byte_at(index) != 0)
                    return false;
            }
            return true;
        }
        if (lead <= 0x7fu) {
            ++index;
            continue;
        }
        if (lead >= 0xc2u && lead <= 0xdfu) {
            if (index + 1 >= identity.size() || !continuation(index + 1))
                return false;
            index += 2;
            continue;
        }
        if (lead >= 0xe0u && lead <= 0xefu) {
            if (index + 2 >= identity.size())
                return false;
            const std::uint8_t second = byte_at(index + 1);
            const bool second_valid = lead == 0xe0u   ? second >= 0xa0u && second <= 0xbfu
                                      : lead == 0xedu ? second >= 0x80u && second <= 0x9fu
                                                      : second >= 0x80u && second <= 0xbfu;
            if (!second_valid || !continuation(index + 2))
                return false;
            index += 3;
            continue;
        }
        if (lead >= 0xf0u && lead <= 0xf4u) {
            if (index + 3 >= identity.size())
                return false;
            const std::uint8_t second = byte_at(index + 1);
            const bool second_valid = lead == 0xf0u   ? second >= 0x90u && second <= 0xbfu
                                      : lead == 0xf4u ? second >= 0x80u && second <= 0x8fu
                                                      : second >= 0x80u && second <= 0xbfu;
            if (!second_valid || !continuation(index + 2) || !continuation(index + 3)) {
                return false;
            }
            index += 4;
            continue;
        }
        return false;
    }
    return false;
}

/// Check that environment fields match the backend and fit SQL report types.
/// CPU routes require empty GPU fields. GPU routes require device and driier
/// identities plus cache records consistent with the selected cache  policy.
[[nodiscard]] constexpr bool
is_environment_valid_for_backend(const execution_environment_snapshot& environment,
                                 executed_backend backend) noexcept {
    if (environment.is_mixed_invalid || environment.effective_cpu_workers == 0 ||
        environment.effective_cpu_workers > k_sql_integer_value_max ||
        environment.gpu_pipeline_cache_seed_size > k_sql_bigint_value_max ||
        !execution_environment_snapshot::has_valid_highway_fields(environment)) {
        return false;
    }
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
    if (backend == executed_backend::scalar_cpu || backend == executed_backend::simd_cpu ||
        backend == executed_backend::mixed_cpu) {
#else
    if (backend == executed_backend::scalar_cpu || backend == executed_backend::simd_cpu) {
#endif
        return !environment.is_gpu_environment_available &&
               is_all_zero(environment.gpu_device_uuid) &&
               is_all_zero(environment.gpu_driver_identity) &&
               environment.gpu_pipeline_cache_policy == pipeline_cache_policy::not_applicable &&
               is_all_zero(environment.gpu_pipeline_cache_seed_sha256) &&
               environment.gpu_pipeline_cache_seed_size == 0 &&
               environment.gpu_pipeline_cache_manifest_checksum == 0 &&
               environment.gpu_pipeline_cache_hit == optional_observation_bool::not_applicable;
    }
    if (backend != executed_backend::vulkan_gpu
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
        && backend != executed_backend::hybrid_cpu_vulkan
#endif
    )
        return false;
#if defined(__linux__)
    if (environment.gpu_pipeline_cache_policy == pipeline_cache_policy::native_vulkan) {
        return environment.is_gpu_environment_available &&
               !is_all_zero(environment.gpu_device_uuid) &&
               is_canonical_nonempty_driver_identity(environment.gpu_driver_identity) &&
               is_all_zero(environment.gpu_pipeline_cache_seed_sha256) &&
               environment.gpu_pipeline_cache_seed_size == 0u &&
               environment.gpu_pipeline_cache_manifest_checksum == 0u &&
               environment.gpu_pipeline_cache_hit == optional_observation_bool::false_value;
    }
#endif
    const bool common_gpu_identity_valid =
        environment.is_gpu_environment_available && !is_all_zero(environment.gpu_device_uuid) &&
        is_canonical_nonempty_driver_identity(environment.gpu_driver_identity) &&
        !is_all_zero(environment.gpu_pipeline_cache_seed_sha256) &&
        environment.gpu_pipeline_cache_manifest_checksum != 0;
    if (!common_gpu_identity_valid)
        return false;
    if (environment.gpu_pipeline_cache_policy == pipeline_cache_policy::immutable_preseed_all_hit) {
        return environment.gpu_pipeline_cache_seed_size > 0 &&
               environment.gpu_pipeline_cache_hit == optional_observation_bool::true_value;
    }
    if (environment.gpu_pipeline_cache_policy == pipeline_cache_policy::isolated_miss_diagnostic) {
        return environment.gpu_pipeline_cache_hit == optional_observation_bool::false_value;
    }
    return false;
}

#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
[[nodiscard]] constexpr execution_engine expected_execution_engine(
    executed_backend backend, target_index_source source, pipeline_stage stage) noexcept;
[[nodiscard]] constexpr bool is_stage_coverage_exclusive(
    const stage_coverage& coverage, execution_engine expected_engine) noexcept;

/// Allow scalar or SIMD execution for selectable CPU stages. Fixed stages
/// still require their expected engine, and fallback counts must be zero.
[[nodiscard]] constexpr bool is_mixed_cpu_stage_coverage_exclusive(
    const stage_coverage& coverage, target_index_source source, pipeline_stage stage) noexcept {
    const bool selectable_cpu_stage =
        stage == pipeline_stage::query_sketch || stage == pipeline_stage::target_sketch ||
        stage == pipeline_stage::chain_score || stage == pipeline_stage::chain_backtrack ||
        stage == pipeline_stage::left_extension || stage == pipeline_stage::gap_extension ||
        stage == pipeline_stage::right_extension;
    if (!selectable_cpu_stage) {
        return is_stage_coverage_exclusive(
            coverage, expected_execution_engine(executed_backend::scalar_cpu, source, stage));
    }
    if (coverage.eligible_jobs > 1 || coverage.fallback_jobs != 0)
        return false;
    const std::size_t scalar = engine_index(execution_engine::scalar_cpu);
    const std::size_t simd = engine_index(execution_engine::simd_cpu);
    for (std::size_t i = 0; i < coverage.executed_jobs.size(); ++i) {
        if (coverage.executed_jobs[i] > 1 ||
            (i != scalar && i != simd && coverage.executed_jobs[i] != 0)) {
            return false;
        }
    }
    return coverage.executed_jobs[scalar] + coverage.executed_jobs[simd] ==
           coverage.eligible_jobs;
}
#endif
/// Return the engine expected for this backend, index source and stage.
/// count means the stage must have no eligible or executed jobs. Mixed routes
/// use additional checks for their selectable stages.
[[nodiscard]] constexpr execution_engine expected_execution_engine(executed_backend backend,
                                                                   target_index_source source,
                                                                   pipeline_stage stage) noexcept {
    if (!is_concrete_backend(backend) || !is_concrete_target_index_source(source)) {
        return execution_engine::count;
    }
    const execution_engine backend_engine =
        backend == executed_backend::vulkan_gpu ? execution_engine::vulkan_gpu
        : backend == executed_backend::simd_cpu ? execution_engine::simd_cpu
                                                : execution_engine::scalar_cpu;
    switch (stage) {
    case pipeline_stage::query_sketch:
        return backend_engine;
    case pipeline_stage::target_sketch:
        return source == target_index_source::recomputed ? backend_engine : execution_engine::count;
    case pipeline_stage::target_index_build:
        if (source != target_index_source::recomputed)
            return execution_engine::count;
        return backend == executed_backend::vulkan_gpu ? execution_engine::vulkan_gpu
                                                       : execution_engine::scalar_cpu;
    case pipeline_stage::persisted_index_decode:
        return source == target_index_source::persisted ? execution_engine::host_io
                                                        : execution_engine::count;
    case pipeline_stage::partition_selection:
        if (source != target_index_source::persisted)
            return execution_engine::count;
        return backend == executed_backend::vulkan_gpu ? execution_engine::vulkan_gpu
                                                       : execution_engine::scalar_cpu;
    case pipeline_stage::device_index_upload:
        return source == target_index_source::persisted && backend == executed_backend::vulkan_gpu
                   ? execution_engine::vulkan_gpu
                   : execution_engine::count;
    case pipeline_stage::seed_lookup:
        return backend == executed_backend::vulkan_gpu ? execution_engine::vulkan_gpu
                                                       : execution_engine::scalar_cpu;
    case pipeline_stage::chain_score:
    case pipeline_stage::chain_backtrack:
        return backend_engine;
    case pipeline_stage::region_generation:
    case pipeline_stage::primary_secondary_reduction:
    case pipeline_stage::divergence_estimation:
    case pipeline_stage::mapq:
    case pipeline_stage::target_window_plan:
    case pipeline_stage::cigar_job_generation:
        return backend == executed_backend::vulkan_gpu ? execution_engine::vulkan_gpu
                                                       : execution_engine::scalar_cpu;
    case pipeline_stage::target_window_io:
        return execution_engine::host_io;
    case pipeline_stage::target_window_upload:
        return backend == executed_backend::vulkan_gpu ? execution_engine::vulkan_gpu
                                                       : execution_engine::count;
    case pipeline_stage::left_extension:
    case pipeline_stage::gap_extension:
    case pipeline_stage::right_extension:
        return backend_engine;
    case pipeline_stage::count:
        return execution_engine::count;
    }
    return execution_engine::count;
}

/// Require binary job counts, no fallback and execution by the expected engine.
/// If expected_engine is count, every count must be zero
[[nodiscard]] constexpr bool
is_stage_coverage_exclusive(const stage_coverage& coverage,
                            execution_engine expected_engine) noexcept {
    if (coverage.eligible_jobs > 1 || coverage.fallback_jobs > 1)
        return false;
    for (const std::uint64_t executed : coverage.executed_jobs) {
        if (executed > 1)
            return false;
    }
    if (coverage.fallback_jobs != 0)
        return false;
    if (expected_engine == execution_engine::count) {
        if (coverage.eligible_jobs != 0)
            return false;
        for (const std::uint64_t executed : coverage.executed_jobs) {
            if (executed != 0)
                return false;
        }
        return true;
    }
    for (std::size_t i = 0; i < coverage.executed_jobs.size(); ++i) {
        const std::uint64_t expected =
            i == engine_index(expected_engine) ? coverage.eligible_jobs : 0;
        if (coverage.executed_jobs[i] != expected)
            return false;
    }
    return true;
}

/// Identify preparation stages required even  when a job finds no matches.
/// Query sketching is always required. Target sketch/index creation or persisted
/// index decoding/selection depends on the index source.
[[nodiscard]] constexpr bool is_front_end_eligibility_required(target_index_source source,
                                                               pipeline_stage stage) noexcept {
    if (stage == pipeline_stage::query_sketch)
        return true;
    if (source == target_index_source::recomputed) {
        return stage == pipeline_stage::target_sketch ||
               stage == pipeline_stage::target_index_build;
    }
    if (source == target_index_source::persisted) {
        return stage == pipeline_stage::persisted_index_decode ||
               stage == pipeline_stage::partition_selection;
    }
    return false;
}

#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
/**
 * Check hybrid stage counts against the recorded algorithms. Seed lookup, and
 * chaining may run on Vulkan, followed by CPU backtracking and alignment.
 * Index creation and partition selection allow the host or GPU engine used by
 * the storage path. Counts must fit maximum_jobs and contain no fallwack.
 */
[[nodiscard]] constexpr bool is_hybrid_stage_coverage_exclusive(
    const stage_coverage& coverage,
    target_index_source source,
    pipeline_stage stage,
    minimizer_algorithm_identity minimizer,
    chaining_algorithm_identity chaining,
    base_alignment_algorithm_identity alignment,
    std::uint64_t maximum_jobs = 1u) noexcept {
    execution_engine sketch_engine = execution_engine::count;
    switch (minimizer) {
    case minimizer_algorithm_identity::scalar:
        sketch_engine = execution_engine::scalar_cpu;
        break;
    case minimizer_algorithm_identity::simd_minimap2_reference:
    case minimizer_algorithm_identity::two_stack_nthash:
    case minimizer_algorithm_identity::bounded_nthash_simd:
        sketch_engine = execution_engine::simd_cpu;
        break;
    case minimizer_algorithm_identity::vulkan_minimap_integer:
        sketch_engine = execution_engine::vulkan_gpu;
        break;
    case minimizer_algorithm_identity::none:
    case minimizer_algorithm_identity::mixed_invalid:
        return false;
    }
    execution_engine chain_engine = execution_engine::count;
    switch (chaining) {
    case chaining_algorithm_identity::scalar_minimap2:
    case chaining_algorithm_identity::range_minimum_query:
        chain_engine = execution_engine::scalar_cpu;
        break;
    case chaining_algorithm_identity::simd_minimap2:
        chain_engine = execution_engine::simd_cpu;
        break;
    case chaining_algorithm_identity::vulkan_tiled_dynamic_programming:
        chain_engine = execution_engine::vulkan_gpu;
        break;
    case chaining_algorithm_identity::none:
    case chaining_algorithm_identity::mixed_invalid:
        return false;
    }
    execution_engine alignment_engine = execution_engine::count;
    switch (alignment) {
    case base_alignment_algorithm_identity::ksw2:
        alignment_engine = execution_engine::scalar_cpu;
        break;
    case base_alignment_algorithm_identity::block_aligner:
        alignment_engine = execution_engine::simd_cpu;
        break;
    case base_alignment_algorithm_identity::none:
    case base_alignment_algorithm_identity::vulkan_simple_cigar:
    case base_alignment_algorithm_identity::mixed_invalid:
        return false;
    }
    const bool device_seed = sketch_engine == execution_engine::vulkan_gpu;
    const bool device_chain = chain_engine == execution_engine::vulkan_gpu;
    if ((!device_seed && !device_chain) || !is_concrete_target_index_source(source) ||
        coverage.fallback_jobs != 0u || coverage.eligible_jobs > maximum_jobs) {
        return false;
    }
    execution_engine primary =
        expected_execution_engine(executed_backend::scalar_cpu, source, stage);
    execution_engine alternate = execution_engine::count;
    switch (stage) {
    case pipeline_stage::query_sketch:
        primary = sketch_engine;
        break;
    case pipeline_stage::target_sketch:
        primary = source == target_index_source::recomputed
            ? sketch_engine : execution_engine::count;
        break;
    case pipeline_stage::target_index_build:
        if (device_seed && source == target_index_source::recomputed)
            alternate = execution_engine::vulkan_gpu;
        break;
    case pipeline_stage::partition_selection:
        if (device_seed && source == target_index_source::persisted)
            alternate = execution_engine::vulkan_gpu;
        break;
    case pipeline_stage::device_index_upload:
        primary = device_seed ? execution_engine::vulkan_gpu : execution_engine::count;
        break;
    case pipeline_stage::seed_lookup:
        primary = device_seed ? execution_engine::vulkan_gpu : execution_engine::scalar_cpu;
        break;
    case pipeline_stage::chain_score:
        primary = chain_engine;
        break;
    case pipeline_stage::chain_backtrack:
        // Backtrack on the CPU using the scores and predecessors from Vulkan.
        primary = device_chain ? execution_engine::scalar_cpu : chain_engine;
        break;
    case pipeline_stage::left_extension:
    case pipeline_stage::gap_extension:
    case pipeline_stage::right_extension:
        primary = alignment_engine;
        break;
    case pipeline_stage::persisted_index_decode:
    case pipeline_stage::region_generation:
    case pipeline_stage::primary_secondary_reduction:
    case pipeline_stage::divergence_estimation:
    case pipeline_stage::mapq:
    case pipeline_stage::target_window_plan:
    case pipeline_stage::target_window_io:
    case pipeline_stage::target_window_upload:
    case pipeline_stage::cigar_job_generation:
    case pipeline_stage::count:
        break;
    }
    std::uint64_t executed_total = 0u;
    for (std::size_t engine = 0u; engine < coverage.executed_jobs.size(); ++engine) {
        const auto executed = coverage.executed_jobs[engine];
        const bool allowed = engine == engine_index(primary) ||
                             engine == engine_index(alternate);
        if ((!allowed && executed != 0u) ||
            executed > coverage.eligible_jobs - executed_total) {
            return false;
        }
        executed_total += executed;
    }
    return executed_total == coverage.eligible_jobs;
}
#endif

/// Validate one target-query job before adding it to statement statistics.
/// Requested and recorded settings must agree, the  environment must be valid,
/// and stage counts must match the route and target-index source.
[[nodiscard]] constexpr bool
is_job_execution_stats_valid(const pipeline_job_execution_stats& job
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
    , minimizer_algorithm_identity observed_minimizer = minimizer_algorithm_identity::none
    , chaining_algorithm_identity observed_chaining = chaining_algorithm_identity::none
    , base_alignment_algorithm_identity observed_alignment = base_alignment_algorithm_identity::none
    , bool authenticated_query_empty_recomputed_bypass = false
#endif
) noexcept {
    if (!is_concrete_backend(job.requested_backend) ||
        job.requested_backend != job.actual_backend ||
        !is_simd_chaining_policy_identity_valid_for_backend(job.actual_backend,
                                                            job.requested_simd_chaining_policy,
                                                            job.observed_simd_chaining_policy) ||
        !is_simd_alignment_algorithm_identity_valid_for_backend(
            job.actual_backend,
            job.requested_simd_alignment_algorithm,
            job.observed_simd_alignment_algorithm) ||
        !is_concrete_target_index_source(job.requested_target_index_source) ||
        job.requested_target_index_source != job.actual_target_index_source ||
        execution_environment_snapshot::has_no_highway_fields(job.environment) ||
        !is_environment_valid_for_backend(job.environment, job.actual_backend)) {
        return false;
    }
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
    if (authenticated_query_empty_recomputed_bypass &&
        (job.actual_backend != executed_backend::hybrid_cpu_vulkan ||
         job.actual_target_index_source != target_index_source::recomputed ||
         observed_minimizer != minimizer_algorithm_identity::vulkan_minimap_integer)) {
        return false;
    }
#endif
    if (job.logical_target_row > k_sql_bigint_value_max ||
        job.query_ordinal > k_sql_bigint_value_max) {
        return false;
    }
    if (job.actual_target_index_source == target_index_source::persisted) {
        if (job.persisted_generation_id == 0 ||
            job.persisted_generation_id > k_sql_bigint_value_max ||
            job.persisted_source_identity == 0) {
            return false;
        }
    } else if (job.persisted_generation_id != 0 || job.persisted_source_identity != 0) {
        return false;
    }
    for (std::size_t i = 0; i < k_pipeline_stage_count; ++i) {
        const auto stage = static_cast<pipeline_stage>(i);
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
        const execution_engine expected_engine = expected_execution_engine(
            job.actual_backend == executed_backend::mixed_cpu ||
                    job.actual_backend == executed_backend::hybrid_cpu_vulkan
                ? executed_backend::scalar_cpu : job.actual_backend,
            job.actual_target_index_source,
            stage);
        const bool coverage_valid =
            job.actual_backend == executed_backend::hybrid_cpu_vulkan
                ? is_hybrid_stage_coverage_exclusive(job.stages.coverage[i],
                    job.actual_target_index_source, stage, observed_minimizer,
                    observed_chaining, observed_alignment)
            : job.actual_backend == executed_backend::mixed_cpu
                ? is_mixed_cpu_stage_coverage_exclusive(
                      job.stages.coverage[i], job.actual_target_index_source, stage)
                : is_stage_coverage_exclusive(job.stages.coverage[i], expected_engine);
#else
        const execution_engine expected_engine =
            expected_execution_engine(job.actual_backend, job.actual_target_index_source, stage);
        if (!is_stage_coverage_exclusive(job.stages.coverage[i], expected_engine)) {
            return false;
        }
#endif
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
        if (!coverage_valid) {
            return false;
        }
        const bool bypassed_recomputed_target_stage =
            authenticated_query_empty_recomputed_bypass &&
            (stage == pipeline_stage::target_sketch ||
             stage == pipeline_stage::target_index_build);
        if (bypassed_recomputed_target_stage &&
            job.stages.coverage[i].eligible_jobs != 0u) {
            return false;
        }
#endif
        if (expected_engine != execution_engine::count &&
            is_front_end_eligibility_required(job.actual_target_index_source, stage) &&
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
            !bypassed_recomputed_target_stage &&
#endif
            job.stages.coverage[i].eligible_jobs != 1) {
            return false;
        }
    }
    return true;
}

} // namespace bioseq::pipeline_stats
