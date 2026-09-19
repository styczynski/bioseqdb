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

/// @brief Return the stable SQL/report spelling of an optional observation.
/// @param[in] value Optional-observation identity to format.
/// @return Stable report spelling for `value`.
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

/// @brief Return the stable SQL/report spelling of a pipeline-cache policy.
/// @param[in] policy Pipeline-cache policy to format.
/// @return Stable report spelling for `policy`.
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

/// @brief Return whether every byte/element of a fixed stats field is zero.
/// @tparam p_value_type Element type compared with its value-initialized zero.
/// @tparam p_size Fixed number of elements in the field.
/// @param[in] value Fixed field to inspect.
/// @return `true` when every element equals its value-initialized state.
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

    /// @brief Identify the canonical never-captured environment sentinel.
    /// @param[in] value Environment snapshot to inspect.
    /// @return `true` when every environment field has its unset value.
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

    /// @brief Compare every environment identity field except mixed-invalid state.
    /// @param[in] lhs First environment payload.
    /// @param[in] rhs Second environment payload.
    /// @return `true` when all payload identity fields are equal.
    [[nodiscard]] static constexpr bool
    is_payload_equal(const execution_environment_snapshot& lhs,
                     const execution_environment_snapshot& rhs) noexcept {
        return non_highway_is_payload_equal(lhs, rhs) &&
               lhs.highway_compiled_targets == rhs.highway_compiled_targets &&
               lhs.highway_cpu_supported_targets == rhs.highway_cpu_supported_targets &&
               lhs.highway_runnable_targets == rhs.highway_runnable_targets;
    }

    /// @brief Compare environment identity while deferring a zero Highway mask.
    ///
    /// Treat a zero Highway mask as an
    /// observation that has not arrived yet. This identity predicate is used
    /// only while merging independently completed stats shards.
    /// final validation still requires an all-or-none, internally consistent
    /// Highway plan and a singular executed target.
    /// @param[in] lhs First environment payload.
    /// @param[in] rhs Second environment payload.
    /// @return `true` when payload identity matches after zero-mask deferral.
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

    /// @brief Return whether no Highway plan or binding has been attached.
    ///
    /// This sentinel
    /// is valid while constructing a non-Highway portion of an environment,
    /// but partially populated Highway stats are never accepted.
    /// @param[in] value Environment snapshot to inspect.
    /// @return `true` when no Highway plan or executed target is present.
    [[nodiscard]] static constexpr bool
    has_no_highway_fields(const execution_environment_snapshot& value) noexcept {
        return value.highway_compiled_targets == 0 && value.highway_cpu_supported_targets == 0 &&
               value.highway_runnable_targets == 0 && value.highway_executed_target == 0 &&
               !value.is_highway_executed_target_mixed;
    }

    /// @brief Validate the exact plan-mask relation and runtime binding.
    ///
    /// The sole dynamic-dispatch root returns the binding. Once any Highway field is available,
    /// absence,
    /// a multi-bit target, or a target outside the runnable set fails closed.
    /// @param[in] value Environment snapshot to validate.
    /// @return `true` for either no Highway fields or one internally valid plan and binding.
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
    /// @brief Compare every non-Highway environment identity field.
    /// @param[in] lhs First environment payload.
    /// @param[in] rhs Second environment payload.
    /// @return `true` when every non-Highway payload identity field is equal.
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
    /// @brief Adopt the first complete stats and fail closed on absence or drift.
    /// @param[in] rhs Environment snapshot to merge.
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

    /// @brief Record the registry plan and sole runtime-dispatch binding.
    ///
    /// Plan-mask drift is an environment invariant failure. Target
    /// drift is represented explicitly so aggregation never reports a false
    /// singular architecture.
    /// @param[in] plan Runtime dispatch plan observed for the operation.
    /// @param[in] executed_target Exact target bit returned by runtime dispatch.
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

/// @brief Identify an executable, non-sentinel biological backend.
/// @param[in] backend Backend identity to inspect.
/// @return `true` when `backend` names scalar, SIMD, or Vulkan execution.
[[nodiscard]] constexpr bool is_concrete_backend(executed_backend backend) noexcept {
    return backend == executed_backend::scalar_cpu || backend == executed_backend::simd_cpu ||
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
           backend == executed_backend::mixed_cpu ||
           backend == executed_backend::hybrid_cpu_vulkan ||
#endif
           backend == executed_backend::vulkan_gpu;
}

/// @brief Identify a concrete recomputed or persisted index source.
/// @param[in] source Target-index source to inspect.
/// @return `true` when `source` names recomputed or persisted input.
[[nodiscard]] constexpr bool is_concrete_target_index_source(target_index_source source) noexcept {
    return source == target_index_source::recomputed || source == target_index_source::persisted;
}

/// @brief Merge backend identities, exposing concrete disagreement as mixed-invalid.
/// @param[in] lhs Retained backend identity.
/// @param[in] rhs Incoming backend identity.
/// @return Adopted, equal, or mixed-invalid backend identity.
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

/// @brief Merge index-source identities, exposing concrete disagreement as mixed-invalid.
/// @param[in] lhs Retained target-index identity.
/// @param[in] rhs Incoming target-index identity.
/// @return Adopted, equal, or mixed-invalid target-index identity.
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

/// @brief Identify a concrete, operation-attestable SIMD chaining policy.
/// @param[in] policy SIMD chaining-policy identity to inspect.
/// @return `true` when `policy` names an executable SIMD chaining policy.
[[nodiscard]] constexpr bool
is_concrete_simd_chaining_policy_identity(simd_chaining_policy_identity policy) noexcept {
    return policy == simd_chaining_policy_identity::exact_visited ||
           policy == simd_chaining_policy_identity::bounded_batch_approximate;
}

/// @brief Merge policy identities without concealing concrete disagreement.
///
/// A concrete disagreement remains visible and
/// revokes aggregate attestation instead of silently choosing either policy.
/// @param[in] lhs Retained SIMD chaining-policy identity.
/// @param[in] rhs Incoming SIMD chaining-policy identity.
/// @return Adopted, equal, or mixed-invalid SIMD chaining identity.
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

/// @brief Return whether the alignment identity names the concrete SIMD algorithm.
/// @param[in] algorithm SIMD alignment identity to inspect.
/// @return `true` when `algorithm` names Block Aligner.
[[nodiscard]] constexpr bool is_concrete_simd_alignment_algorithm_identity(
    simd_alignment_algorithm_identity algorithm) noexcept {
    return algorithm == simd_alignment_algorithm_identity::block_aligner;
}

/// @brief Merge SIMD alignment identities, exposing any concrete disagreement.
/// @param[in] lhs Retained SIMD alignment identity.
/// @param[in] rhs Incoming SIMD alignment identity.
/// @return Adopted, equal, or mixed-invalid SIMD alignment identity.
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

/// @brief Validate SIMD alignment identities for a biological backend.
///
/// Only an all-SIMD backend may publish SIMD base-alignment identity, and its
/// requested and operation-observed identities must both be Block Aligner.
/// @param[in] backend Biological backend to validate.
/// @param[in] requested Requested SIMD alignment identity.
/// @param[in] observed Operation-observed SIMD alignment identity.
/// @return `true` when the identities are valid for `backend`.
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

/// @brief Validate SIMD chaining-policy identities for a biological backend.
///
/// A concrete all-SIMD job must retain the same requested and observed
/// policy. Scalar/GPU jobs must carry no SIMD-policy identity at all.
/// @param[in] backend Biological backend to validate.
/// @param[in] requested Requested SIMD chaining-policy identity.
/// @param[in] observed Operation-observed SIMD chaining-policy identity.
/// @return `true` when the identities are valid for `backend`.
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

/// @brief Validate nonempty UTF-8 followed by one NUL and canonical zero padding.
/// @param[in] identity Fixed driver-identity field to validate.
/// @return `true` for canonical, nonempty, zero-padded UTF-8.
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

/// @brief Validate CPU/GPU environment presence, identity, and SQL-bound ranges.
/// @param[in] environment Environment snapshot to validate.
/// @param[in] backend Concrete biological backend expected for the snapshot.
/// @return `true` when `environment` is valid for the concrete `backend`.
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

/// Validate a mixed CPU stage without weakening fixed host/scalar stages.
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
/// @brief Return the sole permitted execution engine for a stage and mode.
///
/// `execution_engine::count`
/// means that the stage must be entirely ineligible in this mode.
/// @param[in] backend Concrete biological backend.
/// @param[in] source Concrete target-index source.
/// @param[in] stage Pipeline stage to classify.
/// @return Required execution engine, or `count` when the stage is ineligible.
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

/// @brief Validate binary per-job counts and exclusive execution-engine attribution.
/// @param[in] coverage Per-job stage coverage to validate.
/// @param[in] expected_engine Sole engine permitted to carry execution.
/// @return `true` when only the expected engine carries binary job coverage.
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

/// @brief Identify stages whose front-end eligibility is unconditional.
///
/// The unconditional front end must execute once even for a zero-match job.
/// Every later stage is data-dependent and may validly report 0/0 while still
/// retaining binary counts and exclusive engine attribution when it does run.
/// @param[in] source Concrete target-index source.
/// @param[in] stage Pipeline stage to classify.
/// @return `true` when the stage must be eligible for every job.
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
 * Validate hybrid coverage against the existing operation-observed identities.
 *
 * No algorithm identity or engine is stored a second time. Index construction
 * and partitioning may be host MPF or device MPS operations; their authenticated
 * layout receipt is checked by the producer. Biological seed/chain/alignment
 * engines and the host tail are exact here, independent of that storage choice.
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
        // Vulkan scores/predecessors feed the existing host backtracking code.
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

/// @brief Validate one complete logical target-query stats record fail-closed.
/// @param[in] job Job-level attestation to validate.
/// @return `true` when identities, environment, coordinates, and stages all agree.
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
