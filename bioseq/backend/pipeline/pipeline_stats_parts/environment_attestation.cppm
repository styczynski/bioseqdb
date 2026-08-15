/**
 * @file pipeline/pipeline_stats_parts/environment_attestation.cppm
 * @brief Execution-environment snapshots and per-job validation.
 */
export module bioseq.pipeline.stats:environment_attestation;

import std;
import :identities;
import bioseq.pipeline.runtime_contract;

export namespace bioseq::pipeline_stats {

/// Tri-state optional observation plus a fail-closed mixed sentinel.
enum class optional_observation_bool : std::uint8_t {
    not_applicable,
    false_value,
    true_value,
    mixed_invalid,
};

/// Pipeline-cache preparation policy observed by the timed GPU runtime.
enum class pipeline_cache_policy : std::uint8_t {
    not_applicable,
    immutable_preseed_all_hit,
    isolated_miss_diagnostic,
    mixed_invalid,
};

/// Return the stable SQL/report spelling of an optional observation.
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

/// Return the stable SQL/report spelling of a pipeline-cache policy.
[[nodiscard]] constexpr std::string_view
pipeline_cache_policy_name(pipeline_cache_policy policy) noexcept {
    switch (policy) {
    case pipeline_cache_policy::not_applicable:
        return "not_applicable";
    case pipeline_cache_policy::immutable_preseed_all_hit:
        return "immutable_preseed_all_hit";
    case pipeline_cache_policy::isolated_miss_diagnostic:
        return "isolated_miss_diagnostic";
    case pipeline_cache_policy::mixed_invalid:
        return "mixed_invalid";
    }
    return "unknown";
}

/// Return whether every byte/element of a fixed stats field is zero.
template <typename p_value_type, std::size_t p_size>
[[nodiscard]] constexpr bool is_all_zero(const std::array<p_value_type, p_size>& value) noexcept {
    for (const p_value_type element : value) {
        if (element != p_value_type{})
            return false;
    }
    return true;
}

/// Exact CPU/GPU execution environment captured by the timed runtime.
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

    /// True only for the canonical never-captured environment sentinel.
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

    /// Compare every environment identity field except mixed-invalid state.
    [[nodiscard]] static constexpr bool
    is_payload_equal(const execution_environment_snapshot& lhs,
                     const execution_environment_snapshot& rhs) noexcept {
        return non_highway_is_payload_equal(lhs, rhs) &&
               lhs.highway_compiled_targets == rhs.highway_compiled_targets &&
               lhs.highway_cpu_supported_targets == rhs.highway_cpu_supported_targets &&
               lhs.highway_runnable_targets == rhs.highway_runnable_targets;
    }

    /// Compare environment identity while treating a zero Highway mask as an
    /// observation that has not arrived yet. This identity predicate is used
    /// only while merging independently completed stats shards.
    /// final validation still requires an all-or-none, internally consistent
    /// Highway plan and a singular executed target.
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

    /// True when no Highway plan or binding has been attached.  This sentinel
    /// is valid while constructing a non-Highway portion of an environment,
    /// but partially populated Highway stats are never accepted.
    [[nodiscard]] static constexpr bool
    has_no_highway_fields(const execution_environment_snapshot& value) noexcept {
        return value.highway_compiled_targets == 0 && value.highway_cpu_supported_targets == 0 &&
               value.highway_runnable_targets == 0 && value.highway_executed_target == 0 &&
               !value.is_highway_executed_target_mixed;
    }

    /// Validate the exact plan-mask relation and the binding returned by the
    /// sole dynamic-dispatch root. Once any Highway field is available, absence,
    /// a multi-bit target, or a target outside the runnable set fails closed.
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
    /// Adopt the first complete stats and fail closed on absence or drift.
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

    /// Record the registry plan and the binding returned by the sole runtime
    /// dispatch. Plan-mask drift is an environment invariant failure. Target
    /// drift is represented explicitly so aggregation never reports a false
    /// singular architecture.
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

/// Complete attestation retained until one logical job is validated.
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

/// True only for an executable, non-sentinel biological backend.
[[nodiscard]] constexpr bool is_concrete_backend(executed_backend backend) noexcept {
    return backend == executed_backend::scalar_cpu || backend == executed_backend::simd_cpu ||
           backend == executed_backend::vulkan_gpu;
}

/// True only for a concrete recomputed or persisted index source.
[[nodiscard]] constexpr bool is_concrete_target_index_source(target_index_source source) noexcept {
    return source == target_index_source::recomputed || source == target_index_source::persisted;
}

/// Merge backend identities. Any concrete disagreement becomes mixed-invalid.
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

/// Merge index-source identities. Any concrete disagreement becomes mixed-invalid.
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

/// True only for one concrete, operation-attestable SIMD chaining policy.
[[nodiscard]] constexpr bool
is_concrete_simd_chaining_policy_identity(simd_chaining_policy_identity policy) noexcept {
    return policy == simd_chaining_policy_identity::exact_visited ||
           policy == simd_chaining_policy_identity::bounded_batch_approximate;
}

/// Merge policy identities. A concrete disagreement remains visible and
/// revokes aggregate attestation instead of silently choosing either policy.
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

[[nodiscard]] constexpr bool is_concrete_simd_alignment_algorithm_identity(
    simd_alignment_algorithm_identity algorithm) noexcept {
    return algorithm == simd_alignment_algorithm_identity::block_aligner;
}

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

/// Only an all-SIMD backend may publish SIMD base-alignment identity, and its
/// requested and operation-observed identities must both be Block Aligner.
[[nodiscard]] constexpr bool is_simd_alignment_algorithm_identity_valid_for_backend(
    executed_backend backend,
    simd_alignment_algorithm_identity requested,
    simd_alignment_algorithm_identity observed) noexcept {
    if (backend == executed_backend::simd_cpu) {
        return is_concrete_simd_alignment_algorithm_identity(requested) && requested == observed;
    }
    return requested == simd_alignment_algorithm_identity::none &&
           observed == simd_alignment_algorithm_identity::none;
}

/// A concrete all-SIMD job must retain the same requested and observed
/// policy. Scalar/GPU jobs must carry no SIMD-policy identity at all.
[[nodiscard]] constexpr bool is_simd_chaining_policy_identity_valid_for_backend(
    executed_backend backend,
    simd_chaining_policy_identity requested,
    simd_chaining_policy_identity observed) noexcept {
    if (backend == executed_backend::simd_cpu) {
        return is_concrete_simd_chaining_policy_identity(requested) && requested == observed;
    }
    return requested == simd_chaining_policy_identity::none &&
           observed == simd_chaining_policy_identity::none;
}

/// Validate nonempty UTF-8 followed by one NUL and canonical zero padding.
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

/// Validate CPU/GPU environment presence, identity, and SQL-bound ranges.
[[nodiscard]] constexpr bool
is_environment_valid_for_backend(const execution_environment_snapshot& environment,
                                 executed_backend backend) noexcept {
    if (environment.is_mixed_invalid || environment.effective_cpu_workers == 0 ||
        environment.effective_cpu_workers > k_sql_integer_value_max ||
        environment.gpu_pipeline_cache_seed_size > k_sql_bigint_value_max ||
        !execution_environment_snapshot::has_valid_highway_fields(environment)) {
        return false;
    }
    if (backend == executed_backend::scalar_cpu || backend == executed_backend::simd_cpu) {
        return !environment.is_gpu_environment_available &&
               is_all_zero(environment.gpu_device_uuid) &&
               is_all_zero(environment.gpu_driver_identity) &&
               environment.gpu_pipeline_cache_policy == pipeline_cache_policy::not_applicable &&
               is_all_zero(environment.gpu_pipeline_cache_seed_sha256) &&
               environment.gpu_pipeline_cache_seed_size == 0 &&
               environment.gpu_pipeline_cache_manifest_checksum == 0 &&
               environment.gpu_pipeline_cache_hit == optional_observation_bool::not_applicable;
    }
    if (backend != executed_backend::vulkan_gpu)
        return false;
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

/// Return the sole permitted engine for a stage/mode. execution_engine::count
/// means that the stage must be entirely ineligible in this mode.
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

/// Validate binary per-job counts and exclusive execution-engine attribution.
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

/// The unconditional front end must execute once even for a zero-match job.
/// Every later stage is data-dependent and may validly report 0/0 while still
/// retaining binary counts and exclusive engine attribution when it does run.
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

/// Fail-closed validator for one complete logical target-query stats.
[[nodiscard]] constexpr bool
is_job_execution_stats_valid(const pipeline_job_execution_stats& job) noexcept {
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
        const execution_engine expected_engine =
            expected_execution_engine(job.actual_backend, job.actual_target_index_source, stage);
        if (!is_stage_coverage_exclusive(job.stages.coverage[i], expected_engine)) {
            return false;
        }
        if (expected_engine != execution_engine::count &&
            is_front_end_eligibility_required(job.actual_target_index_source, stage) &&
            job.stages.coverage[i].eligible_jobs != 1) {
            return false;
        }
    }
    return true;
}

} // namespace bioseq::pipeline_stats
