/**
 * @file pipeline/pipeline_stats_parts/persisted_gpu_operations.cppm
 * @brief Persisted-GPU operation receipts, authorization, and aggregation.
 */
export module bioseq.pipeline.stats:persisted_gpu_operations;

import std;
import :aggregation;
import bioseq.gpu.vulkan.runtime;
import bioseq.common.checked_arithmetic;
import bioseq.parallelism.memory_admission;

export namespace bioseq::pipeline_stats {

/// Fixed publication equation checked by the PostgreSQL owner before one
/// persisted grouped-SoA GPU wave becomes visible.  `consumed_jobs` alone is not
/// authority: a mismatched expectation deliberately burns its record, so a
/// stats may be fully consumed while still being ineligible for publication.
struct persisted_grouped_soa_coordinate_publication_stats {
    std::uint64_t expected_jobs = 0;
    std::uint64_t validated_jobs = 0;
    std::uint64_t consumed_jobs = 0;
    std::uint64_t accepted_jobs = 0;
    std::uint64_t burned_jobs = 0;
    std::uint64_t pending_jobs = 0;
    std::uint64_t staged_jobs = 0;
};

/// @brief Validate the exact persisted grouped-SoA coordinate publication equation.
/// @param[in] counts Publication counters to validate.
/// @return `true` when every publication count proves complete, unburned staging.
[[nodiscard]] constexpr bool is_persisted_grouped_soa_coordinate_publication_consistent(
    const persisted_grouped_soa_coordinate_publication_stats& counts) noexcept {
    return counts.expected_jobs != 0 && counts.expected_jobs <= k_sql_bigint_value_max &&
           counts.validated_jobs == counts.expected_jobs &&
           counts.consumed_jobs == counts.expected_jobs &&
           counts.accepted_jobs == counts.expected_jobs && counts.burned_jobs == 0 &&
           counts.pending_jobs == 0 && counts.staged_jobs == counts.expected_jobs;
}

static_assert(std::is_trivially_copyable_v<pipeline_execution_stats>);
static_assert(std::is_standard_layout_v<pipeline_execution_stats>);

/// Maximum statement-local persisted-GPU records retained for SQL stats.
/// This is the production dispatch ceiling, not a silently growing cache.
inline constexpr std::uint64_t k_persisted_gpu_operation_capacity = 8192u;

/// One causal queue submission, represented without Vulkan handles/pointers.
struct persisted_gpu_submission_stats {
    std::uint64_t operation_identity = 0u;
    std::uint64_t ordinal = 0u;
    std::uint64_t submit_wait_identity = 0u;

    /// @brief Return whether the submission carries complete SQL-bound causal identities.
    /// @return `true` when every causal identity is present and the ordinal is SQL-bound.
    [[nodiscard]] constexpr bool is_internally_consistent() const noexcept {
        return operation_identity != 0u && ordinal != 0u && ordinal <= k_sql_bigint_value_max &&
               submit_wait_identity != 0u;
    }
    [[nodiscard]] constexpr bool
    operator==(const persisted_gpu_submission_stats&) const noexcept = default;
};

/// Immutable numeric Vulkan/cache identity copied from the runtime stats.
struct persisted_gpu_environment_snapshot {
    std::array<std::uint8_t, 16> device_uuid{};
    std::uint32_t api_version = 0u;
    std::uint32_t driver_version = 0u;
    std::uint32_t vendor_id = 0u;
    std::uint32_t device_id = 0u;
    std::array<std::uint8_t, 16> pipeline_cache_uuid{};
    std::array<std::uint8_t, 32> seed_sha256{};
    std::uint64_t seed_size = 0u;
    std::uint64_t manifest_checksum = 0u;

    /// @brief Validate presence and bounds of every immutable Vulkan and cache identity.
    /// @return `true` when all environment identities satisfy the persisted-GPU contract.
    [[nodiscard]] bool is_internally_consistent() const noexcept {
        return api_version != 0u && driver_version != 0u && seed_size != 0u &&
               seed_size <= k_sql_bigint_value_max && manifest_checksum != 0u &&
               std::any_of(device_uuid.begin(),
                           device_uuid.end(),
                           [](const std::uint8_t value) { return value != 0u; }) &&
               std::any_of(pipeline_cache_uuid.begin(),
                           pipeline_cache_uuid.end(),
                           [](const std::uint8_t value) { return value != 0u; }) &&
               std::any_of(seed_sha256.begin(), seed_sha256.end(), [](const std::uint8_t value) {
                   return value != 0u;
               });
    }
    [[nodiscard]] constexpr bool
    operator==(const persisted_gpu_environment_snapshot&) const noexcept = default;
};

inline constexpr std::size_t k_persisted_gpu_helper_build_identity_capacity = 128u;
inline constexpr std::size_t k_persisted_gpu_helper_source_capacity = 32u;

/// Process-wide immutable Metal archive qualification copied without paths,
/// file descriptors, API handles or pointers. Text fields are bounded ASCII
/// images so the admitted operation chunk remains a fixed allocation.
struct persisted_gpu_cache_validation_result {
    std::uint32_t qualification_wire_revision = 0u;
    std::uint64_t identity = 0u;
    std::uint64_t archive_count = 0u;
    std::uint64_t archive_bytes = 0u;
    std::uint64_t archive_checksum = 0u;
    std::uint64_t receipt_count = 0u;
    std::uint64_t hash_passes = 0u;
    std::uint64_t hashed_bytes = 0u;
    std::uint64_t private_files_created = 0u;
    std::uint64_t private_files_unlinked = 0u;
    std::uint64_t live_private_paths = 0u;
    std::uint64_t pipeline_filesystem_hash_passes = 0u;
    std::uint64_t pipeline_filesystem_hashed_bytes = 0u;
    std::uint32_t helper_wire_revision = 0u;
    std::array<char, k_persisted_gpu_helper_build_identity_capacity> helper_build_identity{};
    std::uint32_t helper_build_identity_size = 0u;
    std::array<char, k_persisted_gpu_helper_source_capacity> helper_source{};
    std::uint32_t helper_source_size = 0u;
    std::uint64_t helper_executable_size = 0u;
    std::array<std::uint8_t, 32> helper_executable_sha256{};
    std::uint64_t helper_metal_registry_id = 0u;
    std::array<std::uint8_t, 16> device_uuid{};
    std::uint32_t api_version = 0u;
    std::uint32_t driver_version = 0u;
    std::uint32_t vendor_id = 0u;
    std::uint32_t device_id = 0u;
    std::array<std::uint8_t, 16> pipeline_cache_uuid{};
    std::uint64_t seed_receipt_identity = 0u;
    std::array<std::uint8_t, 32> seed_sha256{};
    std::uint64_t seed_size = 0u;
    std::uint64_t manifest_checksum = 0u;
    bool is_saturated = false;

    /// @brief Return the validated helper build identity without its zero padding.
    /// @return View over the bounded helper build identity bytes.
    [[nodiscard]] std::string_view helper_build_identity_text() const noexcept {
        return {helper_build_identity.data(), helper_build_identity_size};
    }
    /// @brief Return the validated helper source identity without its zero padding.
    /// @return View over the bounded helper source identity bytes.
    [[nodiscard]] std::string_view helper_source_text() const noexcept {
        return {helper_source.data(), helper_source_size};
    }

    /// @brief Validate the complete immutable archive, helper, device, and cache receipt.
    /// @return `true` when every receipt field satisfies the fixed qualification contract.
    [[nodiscard]] bool is_internally_consistent() const noexcept {
        const auto nonzero = [](const auto& p_bytes) noexcept {
            return std::any_of(
                p_bytes.begin(), p_bytes.end(), [](const auto p_value) { return p_value != 0u; });
        };
        const auto safe_text = [](const auto& p_bytes, const std::uint32_t p_size) noexcept {
            if (p_size == 0u || p_size > p_bytes.size())
                return false;
            for (std::size_t i = 0u; i < p_size; ++i) {
                const auto value = static_cast<unsigned char>(p_bytes[i]);
                if (value < 0x20u || value > 0x7eu || value == static_cast<unsigned char>('"') ||
                    value == static_cast<unsigned char>('\\')) {
                    return false;
                }
            }
            return std::all_of(p_bytes.begin() + p_size, p_bytes.end(), [](const char value) {
                return value == '\0';
            });
        };
        return qualification_wire_revision == 2u && identity != 0u && archive_count != 0u &&
               archive_count <= k_sql_bigint_value_max && archive_bytes != 0u &&
               archive_bytes <= k_sql_bigint_value_max && archive_checksum != 0u &&
               receipt_count == archive_count && hash_passes == 1u &&
               hashed_bytes == archive_bytes && private_files_created == archive_count &&
               private_files_unlinked == archive_count && live_private_paths == 0u &&
               pipeline_filesystem_hash_passes == 0u && pipeline_filesystem_hashed_bytes == 0u &&
               helper_wire_revision == 2u &&
               safe_text(helper_build_identity, helper_build_identity_size) &&
               helper_build_identity_text() == "bioseq_gpu_archive_builder" &&
               safe_text(helper_source, helper_source_size) &&
               helper_source_text() == "installed-sibling" && helper_executable_size != 0u &&
               helper_executable_size <= k_sql_bigint_value_max &&
               nonzero(helper_executable_sha256) && helper_metal_registry_id != 0u &&
               nonzero(device_uuid) && api_version != 0u && driver_version != 0u &&
               nonzero(pipeline_cache_uuid) && seed_receipt_identity != 0u &&
               nonzero(seed_sha256) && seed_size != 0u && seed_size <= k_sql_bigint_value_max &&
               manifest_checksum != 0u && !is_saturated;
    }
    [[nodiscard]] constexpr bool
    operator==(const persisted_gpu_cache_validation_result&) const noexcept = default;
};

/// Report-once cache counters attached to the is_accepted operation authority.
struct persisted_gpu_cache_metrics {
    std::uint64_t token_identity = 0u;
    std::uint64_t pipeline_cache_loads = 0u;
    std::uint64_t pipeline_cache_seed_bytes = 0u;
    std::uint64_t pipeline_cache_load_ns = 0u;
    std::uint64_t pipeline_creates = 0u;
    std::uint64_t pipeline_creation_ns = 0u;

    /// @brief Validate the report-once cache counter shape and SQL bounds.
    /// @return `true` when the cache token and optional counter groups are valid.
    [[nodiscard]] constexpr bool is_internally_consistent() const noexcept {
        const bool load_shape =
            pipeline_cache_loads == 0u
                ? pipeline_cache_seed_bytes == 0u && pipeline_cache_load_ns == 0u
                : pipeline_cache_loads == 1u && pipeline_cache_seed_bytes != 0u &&
                      pipeline_cache_load_ns != 0u;
        const bool create_shape =
            pipeline_creates == 0u ? pipeline_creation_ns == 0u : pipeline_creation_ns != 0u;
        return token_identity != 0u && load_shape && create_shape &&
               pipeline_cache_loads <= k_sql_bigint_value_max &&
               pipeline_cache_seed_bytes <= k_sql_bigint_value_max &&
               pipeline_cache_load_ns <= k_sql_bigint_value_max &&
               pipeline_creates <= k_sql_bigint_value_max &&
               pipeline_creation_ns <= k_sql_bigint_value_max;
    }
    [[nodiscard]] constexpr bool
    operator==(const persisted_gpu_cache_metrics&) const noexcept = default;
};

/// Coordinate result copied after the runtime's status/readback checks.
struct persisted_gpu_coordinate_result_stats {
    std::uint64_t query_start = 0u;
    std::uint64_t query_end = 0u;
    std::uint64_t target_start = 0u;
    std::uint64_t target_end = 0u;
    std::uint64_t match_length = 0u;
    std::uint64_t block_length = 0u;
    std::int32_t chain_score = 0;
    std::int32_t seed_count = 0;
    bool is_reverse = false;
    bool is_valid = false;

    /// @brief Validate a positive coordinate result against its query and target extents.
    /// @param[in] query_length Logical query length in bases.
    /// @param[in] target_length Logical target length in bases.
    /// @return `true` when the coordinate is positive, bounded, and length-consistent.
    [[nodiscard]] constexpr bool
    is_internally_consistent(const std::uint64_t query_length,
                             const std::uint64_t target_length) const noexcept {
        return is_valid && chain_score > 0 && seed_count > 0 && query_start < query_end &&
               query_end <= query_length && target_start < target_end &&
               target_end <= target_length && match_length != 0u && match_length <= block_length &&
               match_length <= query_end - query_start &&
               match_length <= target_end - target_start &&
               block_length <= query_end - query_start + target_end - target_start &&
               query_start <= k_sql_bigint_value_max && query_end <= k_sql_bigint_value_max &&
               target_start <= k_sql_bigint_value_max && target_end <= k_sql_bigint_value_max &&
               match_length <= k_sql_bigint_value_max && block_length <= k_sql_bigint_value_max;
    }
};

/// Full-stage causal statistics for one logical target/query coordinate result.
struct persisted_gpu_job_stats {
    std::uint64_t logical_target_row = 0u;
    std::uint64_t logical_query_ordinal = 0u;
    std::uint32_t query_index = 0u;
    std::uint64_t query_length = 0u;
    std::uint64_t target_length = 0u;
    std::uint64_t query_minimizer_count = 0u;
    std::uint64_t query_tile_count = 0u;
    std::uint64_t query_count_dispatches = 0u;
    std::uint64_t query_scan_dispatches = 0u;
    std::uint64_t query_emit_dispatches = 0u;
    std::uint64_t query_completed_roundtrips = 0u;
    std::uint64_t direct_count_dispatches = 0u;
    std::uint64_t direct_scan_dispatches = 0u;
    std::uint64_t anchor_count = 0u;
    std::uint64_t direct_emit_dispatches = 0u;
    std::uint64_t sort_operations = 0u;
    std::uint64_t range_build_dispatches = 0u;
    std::uint64_t range_count = 0u;
    std::uint64_t chain_dispatches = 0u;
    std::uint64_t chain_backtrack_dispatches = 0u;
    std::uint64_t coordinate_dispatches = 0u;
    std::array<persisted_gpu_submission_stats, 4> submissions{};
    std::uint64_t count_status_checks = 0u;
    std::uint64_t emit_status_checks = 0u;
    std::uint32_t status_or = 0u;
    std::uint64_t result_readbacks = 0u;
    std::uint64_t coordinate_vote_dispatches = 0u;
    std::uint64_t cpu_or_simd_biological_calls = 0u;
    std::uint64_t recompute_calls = 0u;
    std::uint64_t fallback_calls = 0u;
    persisted_gpu_coordinate_result_stats coordinate{};

    /// @brief Validate the causal dispatch, submission, and coordinate record for one job.
    /// @param[in] shape_operation_identity Required identity for every shape submission.
    /// @return `true` when all fixed dispatch counts and causal identities are consistent.
    [[nodiscard]] constexpr bool
    is_internally_consistent(const std::uint64_t shape_operation_identity) const noexcept {
        if (logical_target_row > k_sql_bigint_value_max ||
            logical_query_ordinal > k_sql_bigint_value_max || query_length == 0u ||
            query_length > k_sql_bigint_value_max || target_length == 0u ||
            target_length > k_sql_bigint_value_max || query_minimizer_count == 0u ||
            query_minimizer_count > k_sql_bigint_value_max || query_tile_count == 0u ||
            query_tile_count > k_sql_bigint_value_max || query_count_dispatches != 1u ||
            query_scan_dispatches != 3u || query_emit_dispatches != 1u ||
            query_completed_roundtrips != 2u || direct_count_dispatches != 1u ||
            direct_scan_dispatches != 3u || anchor_count == 0u ||
            anchor_count > k_sql_bigint_value_max || direct_emit_dispatches != 1u ||
            sort_operations != 1u || range_build_dispatches != 1u || range_count == 0u ||
            range_count > k_sql_bigint_value_max || chain_dispatches != 1u ||
            chain_backtrack_dispatches != 1u || coordinate_dispatches != 1u ||
            count_status_checks != 1u || emit_status_checks != 1u || status_or != 0u ||
            result_readbacks != 1u || coordinate_vote_dispatches != 0u ||
            cpu_or_simd_biological_calls != 0u || recompute_calls != 0u || fallback_calls != 0u ||
            !coordinate.is_internally_consistent(query_length, target_length)) {
            return false;
        }
        for (std::size_t i = 0u; i < submissions.size(); ++i) {
            if (!submissions[i].is_internally_consistent() ||
                submissions[i].operation_identity != shape_operation_identity) {
                return false;
            }
            for (std::size_t prior = 0u; prior < i; ++prior) {
                if (submissions[prior].ordinal == submissions[i].ordinal ||
                    submissions[prior].submit_wait_identity ==
                        submissions[i].submit_wait_identity) {
                    return false;
                }
            }
        }
        return true;
    }
};

/// One publication-wave envelope. The final cache delta may be replaced only
/// by the runtime authority immediately before allocation-free operation linking.
struct persisted_gpu_operation_stats {
    std::uint64_t operation_identity = 0u;
    persisted_grouped_soa_coordinate_publication_stats publication{};
    std::uint64_t published_jobs = 0u;
    std::uint64_t publication_commits = 0u;
    std::uint64_t query_pipeline_receipt_count = 0u;
    std::uint64_t shape_pipeline_receipt_count = 0u;
    std::uint64_t submission_count = 0u;
    std::uint64_t query_pipeline_operation_identity = 0u;
    std::uint64_t shape_pipeline_operation_identity = 0u;
    std::array<std::uint64_t, 2> query_submission_ordinals{};
    std::uint64_t seed_receipt_identity = 0u;
    std::uint64_t generation_id = 0u;
    std::uint64_t source_identity = 0u;
    std::uint64_t cold_load_identity = 0u;
    persisted_gpu_environment_snapshot environment{};
    persisted_gpu_cache_validation_result cache_qualification{};
    persisted_gpu_cache_metrics cache_metrics{};

    /// @brief Validate one operation envelope and all uniquely identified job submissions.
    /// @param[in] jobs Immutable job records owned by this operation.
    /// @return `true` when the publication, cache, environment, and jobs form one operation.
    [[nodiscard]] bool
    is_internally_consistent(const std::span<const persisted_gpu_job_stats> jobs) const noexcept {
        if (operation_identity == 0u ||
            !is_persisted_grouped_soa_coordinate_publication_consistent(publication) ||
            published_jobs != publication.expected_jobs || publication_commits != 1u ||
            query_pipeline_receipt_count != 3u || shape_pipeline_receipt_count < 7u ||
            shape_pipeline_receipt_count > 10u || submission_count < 6u ||
            submission_count > k_sql_bigint_value_max || query_pipeline_operation_identity == 0u ||
            shape_pipeline_operation_identity == 0u ||
            shape_pipeline_operation_identity != operation_identity ||
            query_pipeline_operation_identity == shape_pipeline_operation_identity ||
            query_submission_ordinals[0] == 0u || query_submission_ordinals[1] == 0u ||
            query_submission_ordinals[0] == query_submission_ordinals[1] ||
            query_submission_ordinals[0] > k_sql_bigint_value_max ||
            query_submission_ordinals[1] > k_sql_bigint_value_max || seed_receipt_identity == 0u ||
            generation_id == 0u || generation_id > k_sql_bigint_value_max ||
            source_identity == 0u || cold_load_identity == 0u ||
            !environment.is_internally_consistent() ||
            !cache_qualification.is_internally_consistent() ||
            cache_qualification.device_uuid != environment.device_uuid ||
            cache_qualification.api_version != environment.api_version ||
            cache_qualification.driver_version != environment.driver_version ||
            cache_qualification.vendor_id != environment.vendor_id ||
            cache_qualification.device_id != environment.device_id ||
            cache_qualification.pipeline_cache_uuid != environment.pipeline_cache_uuid ||
            cache_qualification.seed_receipt_identity != seed_receipt_identity ||
            cache_qualification.seed_sha256 != environment.seed_sha256 ||
            cache_qualification.seed_size != environment.seed_size ||
            cache_qualification.manifest_checksum != environment.manifest_checksum ||
            !cache_metrics.is_internally_consistent() ||
            cache_metrics.token_identity != operation_identity ||
            jobs.size() != publication.expected_jobs) {
            return false;
        }
        std::uint64_t shape_ordinal_count = 0u;
        for (std::size_t job_index = 0u; job_index < jobs.size(); ++job_index) {
            const persisted_gpu_job_stats& job = jobs[job_index];
            if (!job.is_internally_consistent(shape_pipeline_operation_identity))
                return false;
            for (std::size_t prior = 0u; prior < job_index; ++prior) {
                if (jobs[prior].logical_target_row == job.logical_target_row &&
                    jobs[prior].logical_query_ordinal == job.logical_query_ordinal) {
                    return false;
                }
            }
            for (std::size_t submission_index = 0u; submission_index < job.submissions.size();
                 ++submission_index) {
                const auto& submission = job.submissions[submission_index];
                bool previously_seen = false;
                for (std::size_t prior_job = 0u; prior_job <= job_index && !previously_seen;
                     ++prior_job) {
                    const std::size_t prior_submission_limit =
                        prior_job == job_index ? submission_index
                                               : jobs[prior_job].submissions.size();
                    for (std::size_t prior_submission = 0u;
                         prior_submission < prior_submission_limit;
                         ++prior_submission) {
                        const auto& prior = jobs[prior_job].submissions[prior_submission];
                        const bool same_ordinal = prior.ordinal == submission.ordinal;
                        const bool same_wait =
                            prior.submit_wait_identity == submission.submit_wait_identity;
                        if ((same_ordinal || same_wait) &&
                            !(same_ordinal && same_wait && prior_submission == submission_index)) {
                            return false;
                        }
                        previously_seen = same_ordinal;
                        if (previously_seen)
                            break;
                    }
                }
                if (!previously_seen) {
                    if (shape_ordinal_count == k_sql_bigint_value_max) {
                        return false;
                    }
                    ++shape_ordinal_count;
                }
            }
        }
        return submission_count == 2u + shape_ordinal_count;
    }
};

class persisted_gpu_operation_accumulator;

/// Pre-finalizer authority proving one sealed chunk can be linked without any
/// later allocation, duplicate search, range check, or capacity decision.
class persisted_gpu_operation_append_authorization {
  public:
    persisted_gpu_operation_append_authorization() = default;
    persisted_gpu_operation_append_authorization(
        const persisted_gpu_operation_append_authorization&) = delete;
    persisted_gpu_operation_append_authorization&
    operator=(const persisted_gpu_operation_append_authorization&) = delete;
    /// @brief Move the single-use authorization and invalidate the source.
    /// @param[in,out] other Authorization consumed into this object.
    persisted_gpu_operation_append_authorization(
        persisted_gpu_operation_append_authorization&& other) noexcept
        : owner_(std::exchange(other.owner_, nullptr)) {}
    /// @brief Replace this authorization with the source authority and invalidate the source.
    /// @param[in,out] other Authorization consumed into this object.
    /// @return This authorization after replacement.
    persisted_gpu_operation_append_authorization&
    operator=(persisted_gpu_operation_append_authorization&& other) noexcept {
        if (this != &other) {
            owner_ = std::exchange(other.owner_, nullptr);
        }
        return *this;
    }

    /// @brief Return whether this token still authorizes an originating-accumulator append.
    /// @return `true` while this token retains an originating accumulator.
    [[nodiscard]] bool is_valid() const noexcept {
        return owner_ != nullptr;
    }

  private:
    /// @brief Mint an append authorization bound to one accumulator.
    /// @param[in] owner Accumulator authorized for one append.
    explicit persisted_gpu_operation_append_authorization(
        persisted_gpu_operation_accumulator& owner) noexcept
        : owner_(&owner) {}

    persisted_gpu_operation_accumulator* owner_ = nullptr;
    friend class persisted_gpu_operation_accumulator;
};

/// One admitted operation chunk. The job vector dies before its permit, and
/// the operation chain removes next links iteratively before destroying each chunk.
class persisted_gpu_operation_chunk {
  public:
    persisted_gpu_operation_chunk() = default;
    persisted_gpu_operation_chunk(const persisted_gpu_operation_chunk&) = delete;
    persisted_gpu_operation_chunk& operator=(const persisted_gpu_operation_chunk&) = delete;
    persisted_gpu_operation_chunk(persisted_gpu_operation_chunk&&) = delete;
    persisted_gpu_operation_chunk& operator=(persisted_gpu_operation_chunk&&) = delete;

    /// @brief Admit and allocate one exact operation/job image before finalization.
    /// @param[in] job_count Exact number of job records to retain.
    /// @return Admitted chunk, or null when bounds, admission, or allocation fails.
    [[nodiscard]] static std::unique_ptr<persisted_gpu_operation_chunk>
    prepare(const std::size_t job_count) noexcept {
        using bioseq::common::saturating_add;
        using bioseq::common::saturating_mul;
        if (job_count == 0u || job_count > k_persisted_gpu_operation_capacity ||
            job_count > std::vector<persisted_gpu_job_stats>{}.max_size()) {
            return nullptr;
        }
        const std::uint64_t retained = saturating_add(
            sizeof(persisted_gpu_operation_chunk),
            saturating_mul(static_cast<std::uint64_t>(job_count), sizeof(persisted_gpu_job_stats)));
        const std::uint64_t peak =
            saturating_mul(retained, bioseq::parallelism::k_vector_preallocation_headroom_factor);
        if (retained == (std::numeric_limits<std::uint64_t>::max)() ||
            peak == (std::numeric_limits<std::uint64_t>::max)()) {
            return nullptr;
        }
        bioseq::parallelism::memory_admission_failure failure{};
        auto permit = bioseq::parallelism::memory_admission_controller::instance().try_reserve(
            peak, &failure);
        if (!permit)
            return nullptr;
        try {
            auto chunk = std::make_unique<persisted_gpu_operation_chunk>();
            chunk->capacity_permit_ = std::move(*permit);
            chunk->jobs_.resize(job_count);
            const std::uint64_t actual =
                saturating_add(sizeof(persisted_gpu_operation_chunk),
                               saturating_mul(static_cast<std::uint64_t>(chunk->jobs_.capacity()),
                                              sizeof(persisted_gpu_job_stats)));
            if (actual == (std::numeric_limits<std::uint64_t>::max)() ||
                actual > chunk->capacity_permit_.bytes()) {
                return nullptr;
            }
            chunk->capacity_permit_.shrink_to(actual);
            return chunk;
        } catch (...) {
            return nullptr;
        }
    }

    /// @brief Return mutable operation-envelope storage before sealing.
    /// @return Mutable operation-envelope reference owned by this chunk.
    [[nodiscard]] persisted_gpu_operation_stats& operation() noexcept {
        return operation_;
    }
    /// @brief Return mutable job storage before sealing.
    /// @return Mutable view over all job records owned by this chunk.
    [[nodiscard]] std::span<persisted_gpu_job_stats> jobs() noexcept {
        return jobs_;
    }
    /// @brief Return the immutable operation envelope.
    /// @return Const operation-envelope reference owned by this chunk.
    [[nodiscard]] const persisted_gpu_operation_stats& operation() const noexcept {
        return operation_;
    }
    /// @brief Return the immutable job records.
    /// @return Const view over all job records owned by this chunk.
    [[nodiscard]] std::span<const persisted_gpu_job_stats> jobs() const noexcept {
        return jobs_;
    }
    /// @brief Return the next linked operation chunk, if present.
    /// @return Borrowed next-chunk pointer, or null at the chain tail.
    [[nodiscard]] const persisted_gpu_operation_chunk* next() const noexcept {
        return next_.get();
    }

    /// @brief Freeze populated fields after every full-stage/range/equation check.
    /// @return `true` when the operation and all jobs pass internal validation.
    [[nodiscard]] bool seal() noexcept {
        is_sealed_ = operation_.is_internally_consistent(jobs_);
        return is_sealed_;
    }
    /// @brief Return whether all operation and job invariants passed sealing.
    /// @return `true` after a successful sealing attempt.
    [[nodiscard]] bool is_sealed() const noexcept {
        return is_sealed_;
    }

    /// @brief Publish the runtime-prevalidated final report-once delta.
    ///
    /// The runtime's
    /// claim plan proves componentwise boundedness before its irreversible
    /// report-once commit, so this post-finalizer step is only a fixed write.
    /// @param[in] metrics Runtime-prevalidated final cache delta.
    void publish_cache_metrics(const persisted_gpu_cache_metrics& metrics) noexcept {
        operation_.cache_metrics = metrics;
        has_published_cache_metrics_ = true;
    }
    /// @brief Return whether the final report-once cache delta has been published.
    /// @return `true` after publication of the final cache metrics.
    [[nodiscard]] bool has_published_cache_metrics() const noexcept {
        return has_published_cache_metrics_;
    }

  private:
    // Declared first: jobs/next are destroyed before their admission permit.
    bioseq::parallelism::memory_permit capacity_permit_{};
    std::unique_ptr<persisted_gpu_operation_chunk> next_{};
    std::vector<persisted_gpu_job_stats> jobs_{};
    persisted_gpu_operation_stats operation_{};
    bool has_published_cache_metrics_ = false;
    bool is_sealed_ = false;
    friend class persisted_gpu_operation_accumulator;
};

/// Statement-root-owned accumulator for causal GPU operation statistics.
/// It owns operation chunks directly, while schedulers retain a borrowed pointer.
class persisted_gpu_operation_accumulator {
  public:
    persisted_gpu_operation_accumulator() = default;
    persisted_gpu_operation_accumulator(const persisted_gpu_operation_accumulator&) = delete;
    persisted_gpu_operation_accumulator&
    operator=(const persisted_gpu_operation_accumulator&) = delete;
    /// @brief Move all admitted chunks and accumulator state without allocating.
    /// @param[in,out] other Accumulator consumed into this object.
    persisted_gpu_operation_accumulator(persisted_gpu_operation_accumulator&& other) noexcept {
        move_from(other);
    }
    /// @brief Clear this chain, then move all chunks and state without allocating.
    /// @param[in,out] other Accumulator consumed into this object.
    /// @return This accumulator after replacement.
    persisted_gpu_operation_accumulator&
    operator=(persisted_gpu_operation_accumulator&& other) noexcept {
        if (this != &other) {
            clear_iterative();
            move_from(other);
        }
        return *this;
    }
    /// @brief Iteratively release the operation chain and its admission permits.
    ~persisted_gpu_operation_accumulator() {
        clear_iterative();
    }

    /// @brief Complete all duplicate/capacity/range checks before runtime finalizer.
    /// @param[in] chunk Sealed, unpublished chunk proposed for appending.
    /// @return Single-use append authority, or an invalid token on rejection.
    [[nodiscard]] persisted_gpu_operation_append_authorization
    prepare_append(const std::unique_ptr<persisted_gpu_operation_chunk>& chunk) noexcept {
        if (is_frozen_ || is_invalid_ || chunk == nullptr || !chunk->is_sealed() ||
            chunk->has_published_cache_metrics() ||
            operation_count_ >= k_persisted_gpu_operation_capacity ||
            job_count_ > k_persisted_gpu_operation_capacity - chunk->jobs().size()) {
            invalidate(chunk == nullptr ? 0u : 1u, true);
            return {};
        }
        const auto& operation = chunk->operation();
        for (const persisted_gpu_operation_chunk* existing = head_.get(); existing != nullptr;
             existing = existing->next()) {
            const auto& existing_operation = existing->operation();
            if (existing_operation.operation_identity == operation.operation_identity ||
                existing_operation.query_pipeline_operation_identity ==
                    operation.query_pipeline_operation_identity ||
                existing_operation.query_pipeline_operation_identity ==
                    operation.shape_pipeline_operation_identity ||
                existing_operation.shape_pipeline_operation_identity ==
                    operation.query_pipeline_operation_identity) {
                invalidate(1u, false);
                return {};
            }
            if (existing_operation.generation_id != operation.generation_id ||
                existing_operation.source_identity != operation.source_identity ||
                existing_operation.cold_load_identity != operation.cold_load_identity ||
                existing_operation.seed_receipt_identity != operation.seed_receipt_identity ||
                existing_operation.environment != operation.environment ||
                existing_operation.cache_qualification != operation.cache_qualification) {
                invalidate(1u, false);
                return {};
            }
        }
        return persisted_gpu_operation_append_authorization{*this};
    }

    /// @brief Link allocation-free after accepted runtime authority exists.
    /// @param[in,out] authority Single-use authorization consumed by this append.
    /// @param[in,out] chunk Sealed, published chunk consumed into the chain.
    void commit_append(persisted_gpu_operation_append_authorization&& authority,
                       std::unique_ptr<persisted_gpu_operation_chunk>&& chunk) noexcept {
        if (authority.owner_ != this || chunk == nullptr || !chunk->is_sealed() ||
            !chunk->has_published_cache_metrics()) {
            invalidate(chunk == nullptr ? 0u : 1u, true);
            authority.owner_ = nullptr;
            return;
        }
        persisted_gpu_operation_chunk* appended = chunk.get();
        if (tail_ == nullptr) {
            head_ = std::move(chunk);
        } else {
            tail_->next_ = std::move(chunk);
        }
        tail_ = appended;
        ++operation_count_;
        job_count_ += tail_->jobs_.size();
        authority.owner_ = nullptr;
    }

    /// @brief Freeze at ExecutorFinish before moving into the immutable snapshot.
    void freeze() noexcept {
        is_frozen_ = true;
    }

    /// @brief Return whether the frozen chain is complete, bounded, and fully published.
    /// @return `true` when all linked counts and publication state reconcile.
    [[nodiscard]] bool is_complete() const noexcept {
        if (!is_frozen_ || is_invalid_ || is_saturated_ || dropped_operation_count_ != 0u ||
            operation_count_ > k_persisted_gpu_operation_capacity ||
            job_count_ > k_persisted_gpu_operation_capacity) {
            return false;
        }
        std::uint64_t operations = 0u;
        std::uint64_t jobs = 0u;
        for (const persisted_gpu_operation_chunk* chunk = head_.get(); chunk != nullptr;
             chunk = chunk->next()) {
            if (!chunk->is_sealed() || !chunk->has_published_cache_metrics()) {
                return false;
            }
            ++operations;
            jobs += chunk->jobs().size();
        }
        return operations == operation_count_ && jobs == job_count_;
    }
    /// @brief Return whether a complete accumulator contains at least one operation and job.
    /// @return `true` for a nonempty complete accumulator.
    [[nodiscard]] bool is_internally_consistent() const noexcept {
        return operation_count_ != 0u && job_count_ != 0u && is_complete();
    }
    /// @brief Bind the immutable operation set back to the canonical statement aggregate.
    /// @param[in] stats Canonical statement aggregate to reconcile.
    /// @return `true` when identities and selected operational counters match exactly.
    [[nodiscard]] bool is_pipeline_match(const pipeline_execution_stats& stats) const noexcept {
        if (!is_internally_consistent() ||
            !pipeline_execution_stats::can_retain_full_attestation(stats) ||
            stats.actual_backend != executed_backend::vulkan_gpu ||
            stats.actual_target_index_source != target_index_source::persisted ||
            stats.attempted_alignment_jobs != job_count_ ||
            stats.fully_attested_alignment_jobs != job_count_ ||
            stats.persisted_generation_id == 0u || stats.persisted_source_identity == 0u ||
            !is_environment_valid_for_backend(stats.environment, executed_backend::vulkan_gpu)) {
            return false;
        }
        std::uint64_t cache_loads = 0u;
        std::uint64_t cache_seed_bytes = 0u;
        std::uint64_t cache_load_ns = 0u;
        std::uint64_t pipeline_creates = 0u;
        std::uint64_t pipeline_creation_ns = 0u;
        std::uint64_t seed_dispatches = 0u;
        std::uint64_t chain_dispatches = 0u;
        std::uint64_t backtrack_dispatches = 0u;
        const auto add = [](std::uint64_t& destination, const std::uint64_t value) noexcept {
            if (value > (std::numeric_limits<std::uint64_t>::max)() - destination) {
                return false;
            }
            destination += value;
            return true;
        };
        for (const persisted_gpu_operation_chunk* chunk = head_.get(); chunk != nullptr;
             chunk = chunk->next()) {
            const auto& operation = chunk->operation();
            ::bioseq::gpu::vulkan::vulkan_feature_set identity{};
            identity.api_version = operation.environment.api_version;
            identity.driver_version = operation.environment.driver_version;
            identity.vendor_id = operation.environment.vendor_id;
            identity.device_id = operation.environment.device_id;
            identity.pipeline_cache_uuid = operation.environment.pipeline_cache_uuid;
            if (operation.generation_id != stats.persisted_generation_id ||
                operation.source_identity != stats.persisted_source_identity ||
                operation.environment.device_uuid != stats.environment.gpu_device_uuid ||
                operation.environment.seed_sha256 !=
                    stats.environment.gpu_pipeline_cache_seed_sha256 ||
                operation.environment.seed_size != stats.environment.gpu_pipeline_cache_seed_size ||
                operation.environment.manifest_checksum !=
                    stats.environment.gpu_pipeline_cache_manifest_checksum ||
                !operation.cache_qualification.is_internally_consistent() ||
                operation.cache_qualification.device_uuid != operation.environment.device_uuid ||
                operation.cache_qualification.pipeline_cache_uuid !=
                    operation.environment.pipeline_cache_uuid ||
                operation.cache_qualification.seed_sha256 != operation.environment.seed_sha256 ||
                operation.cache_qualification.seed_size != operation.environment.seed_size ||
                operation.cache_qualification.manifest_checksum !=
                    operation.environment.manifest_checksum ||
                operation.cache_qualification.seed_receipt_identity !=
                    operation.seed_receipt_identity ||
                ::bioseq::gpu::vulkan::canonical_vulkan_driver_identity(identity) !=
                    stats.environment.gpu_driver_identity ||
                !add(cache_loads, operation.cache_metrics.pipeline_cache_loads) ||
                !add(cache_seed_bytes, operation.cache_metrics.pipeline_cache_seed_bytes) ||
                !add(cache_load_ns, operation.cache_metrics.pipeline_cache_load_ns) ||
                !add(pipeline_creates, operation.cache_metrics.pipeline_creates) ||
                !add(pipeline_creation_ns, operation.cache_metrics.pipeline_creation_ns)) {
                return false;
            }
            for (const auto& job : chunk->jobs()) {
                if (!add(seed_dispatches,
                         job.direct_count_dispatches + job.direct_emit_dispatches) ||
                    !add(chain_dispatches, job.chain_dispatches) ||
                    !add(backtrack_dispatches, job.chain_backtrack_dispatches)) {
                    return false;
                }
            }
        }
        return cache_loads == stats.operational.gpu_pipeline_cache_loads &&
               cache_seed_bytes == stats.operational.gpu_pipeline_cache_seed_bytes &&
               cache_load_ns == stats.operational.gpu_pipeline_cache_load_ns &&
               pipeline_creates == stats.operational.gpu_pipeline_creates &&
               pipeline_creation_ns == stats.operational.gpu_pipeline_creation_ns &&
               seed_dispatches == stats.operational.gpu_seed_dispatches &&
               chain_dispatches == stats.operational.gpu_chain_dispatches &&
               backtrack_dispatches == stats.operational.gpu_chain_backtrack_dispatches;
    }
    /// @brief Return whether any accumulator count or capacity decision saturated.
    /// @return Retained saturation state.
    [[nodiscard]] bool is_saturated() const noexcept {
        return is_saturated_;
    }
    /// @brief Return the number of operation records rejected before publication.
    /// @return Saturating dropped-operation count.
    [[nodiscard]] std::uint64_t dropped_operation_count() const noexcept {
        return dropped_operation_count_;
    }
    /// @brief Return the number of linked operation chunks.
    /// @return Linked operation count.
    [[nodiscard]] std::uint64_t operation_count() const noexcept {
        return operation_count_;
    }
    /// @brief Return the number of job records across all linked chunks.
    /// @return Aggregate job count.
    [[nodiscard]] std::uint64_t job_count() const noexcept {
        return job_count_;
    }
    /// @brief Return the first immutable operation chunk, if present.
    /// @return Borrowed head pointer, or null for an empty accumulator.
    [[nodiscard]] const persisted_gpu_operation_chunk* head() const noexcept {
        return head_.get();
    }

  private:
    /// @brief Permanently invalidate the accumulator and count drops saturatingly.
    /// @param[in] dropped Additional rejected-operation count.
    /// @param[in] is_saturated Whether the rejecting condition is a capacity failure.
    void invalidate(const std::uint64_t dropped, const bool is_saturated) noexcept {
        is_invalid_ = true;
        is_saturated_ = is_saturated_ || is_saturated;
        if (dropped > (std::numeric_limits<std::uint64_t>::max)() - dropped_operation_count_) {
            dropped_operation_count_ = (std::numeric_limits<std::uint64_t>::max)();
            is_saturated_ = true;
        } else {
            dropped_operation_count_ += dropped;
        }
    }

    /// @brief Iteratively release the linked chain without recursive destruction.
    void clear_iterative() noexcept {
        tail_ = nullptr;
        while (head_ != nullptr) {
            auto next = std::move(head_->next_);
            head_ = std::move(next);
        }
    }

    /// @brief Transfer the complete chain and reset the source accumulator to empty.
    /// @param[in,out] other Accumulator consumed into this object.
    void move_from(persisted_gpu_operation_accumulator& other) noexcept {
        head_ = std::move(other.head_);
        tail_ = std::exchange(other.tail_, nullptr);
        operation_count_ = std::exchange(other.operation_count_, 0u);
        job_count_ = std::exchange(other.job_count_, 0u);
        dropped_operation_count_ = std::exchange(other.dropped_operation_count_, 0u);
        is_invalid_ = std::exchange(other.is_invalid_, false);
        is_saturated_ = std::exchange(other.is_saturated_, false);
        is_frozen_ = std::exchange(other.is_frozen_, false);
    }

    std::unique_ptr<persisted_gpu_operation_chunk> head_{};
    persisted_gpu_operation_chunk* tail_ = nullptr;
    std::uint64_t operation_count_ = 0u;
    std::uint64_t job_count_ = 0u;
    std::uint64_t dropped_operation_count_ = 0u;
    bool is_invalid_ = false;
    bool is_saturated_ = false;
    bool is_frozen_ = false;
};

static_assert(!std::is_copy_constructible_v<persisted_gpu_operation_accumulator>);
static_assert(std::is_nothrow_move_constructible_v<persisted_gpu_operation_accumulator>);

/// @brief Convert the runtime-minted fixed GPU identity into statement stats.
///
/// The conversion occurs
/// without allocating or inferring cache state from descriptive counters.
/// Only the immutable all-hit policy is_accepted by the persisted operation can
/// become an all-GPU execution environment.
/// @param[in] binding Runtime-minted immutable GPU environment binding.
/// @param[in] effective_cpu_workers Effective statement worker count.
/// @return Validated environment snapshot, or no value when the binding fails closed.
[[nodiscard]] inline std::optional<execution_environment_snapshot>
make_persisted_grouped_soa_gpu_environment_snapshot(
    const ::bioseq::gpu::vulkan::runtime_gpu_execution_environment_snapshot& binding,
    const std::size_t effective_cpu_workers) noexcept {
    using runtime_policy = ::bioseq::gpu::vulkan::runtime_pipeline_cache_policy;
    if (!binding.is_internally_consistent() ||
        binding.cache_policy != runtime_policy::immutable_preseed_all_hit ||
        effective_cpu_workers == 0 || effective_cpu_workers > k_sql_integer_value_max ||
        binding.seed_size > k_sql_bigint_value_max) {
        return std::nullopt;
    }
    execution_environment_snapshot environment{};
    environment.effective_cpu_workers = static_cast<std::uint32_t>(effective_cpu_workers);
    environment.is_gpu_environment_available = binding.gpu_environment_present;
    environment.gpu_device_uuid = binding.gpu_device_uuid;
    environment.gpu_driver_identity = binding.gpu_driver_identity;
    environment.gpu_pipeline_cache_policy = pipeline_cache_policy::immutable_preseed_all_hit;
    environment.gpu_pipeline_cache_seed_sha256 = binding.seed_sha256;
    environment.gpu_pipeline_cache_seed_size = binding.seed_size;
    environment.gpu_pipeline_cache_manifest_checksum = binding.manifest_checksum;
    environment.gpu_pipeline_cache_hit = binding.pipeline_cache_all_hit
                                             ? optional_observation_bool::true_value
                                             : optional_observation_bool::false_value;
    return is_environment_valid_for_backend(environment, executed_backend::vulkan_gpu)
               ? std::optional<execution_environment_snapshot>{environment}
               : std::nullopt;
}

/// @brief Convert the separately owned report-once cache token into an operational delta.
///
/// Descriptive environment stats are intentionally insufficient to
/// manufacture these counters, and the token identity itself is not replayed
/// into the SQL aggregate.
/// @param[in] audit Separately owned report-once cache token and counters.
/// @return Operational cache delta, or no value when the audit is invalid or out of range.
[[nodiscard]] inline std::optional<pipeline_operational_stats>
make_persisted_grouped_soa_gpu_cache_metrics_delta(
    const ::bioseq::gpu::vulkan::runtime_gpu_cache_metrics_audit& audit) noexcept {
    if (!audit.is_internally_consistent() ||
        audit.gpu_pipeline_cache_loads > k_sql_bigint_value_max ||
        audit.gpu_pipeline_cache_seed_bytes > k_sql_bigint_value_max ||
        audit.gpu_pipeline_cache_load_ns > k_sql_bigint_value_max ||
        audit.gpu_pipeline_creates > k_sql_bigint_value_max ||
        audit.gpu_pipeline_creation_ns > k_sql_bigint_value_max) {
        return std::nullopt;
    }
    pipeline_operational_stats delta{};
    delta.gpu_pipeline_cache_loads = audit.gpu_pipeline_cache_loads;
    delta.gpu_pipeline_cache_seed_bytes = audit.gpu_pipeline_cache_seed_bytes;
    delta.gpu_pipeline_cache_load_ns = audit.gpu_pipeline_cache_load_ns;
    delta.gpu_pipeline_creates = audit.gpu_pipeline_creates;
    delta.gpu_pipeline_creation_ns = audit.gpu_pipeline_creation_ns;
    return delta;
}

/// @brief Compare the final GPU environment with its immutable preview.
///
/// The runtime's sealed stats exposes an immutable environment preview before
/// it commits report-once cache bits.  Final authority must reproduce every
/// fixed field exactly. Descriptive equivalence is not enough.
/// @param[in] actual Final runtime environment authority.
/// @param[in] preview Immutable pre-finalizer environment preview.
/// @return `true` when both snapshots are valid and byte-for-field equal.
[[nodiscard]] inline bool is_persisted_grouped_soa_gpu_environment_consistent(
    const ::bioseq::gpu::vulkan::runtime_gpu_execution_environment_snapshot& actual,
    const ::bioseq::gpu::vulkan::runtime_gpu_execution_environment_snapshot& preview) noexcept {
    return actual.is_internally_consistent() && preview.is_internally_consistent() &&
           actual == preview;
}

/// @brief Check final cache metrics against the pre-finalizer maximum claim.
///
/// The pre-finalizer cache audit is a componentwise maximum claim, not the
/// counter delta to publish.  An is_accepted final report-once token may publish
/// only the same operation identity and values bounded by that validated
/// maximum.
/// @param[in] actual Final report-once cache token and counters.
/// @param[in] preview Pre-finalizer componentwise maximum claim.
/// @return `true` when the actual audit is componentwise bounded by the preview.
[[nodiscard]] inline bool is_persisted_grouped_soa_gpu_cache_metrics_bounded_by(
    const ::bioseq::gpu::vulkan::runtime_gpu_cache_metrics_audit& actual,
    const ::bioseq::gpu::vulkan::runtime_gpu_cache_metrics_audit& preview) noexcept {
    return actual.is_componentwise_bounded_by(preview);
}

/// @brief Validate staged persisted-GPU stats and their prospective aggregate.
///
/// Validate the delta
/// without mutating the caller.  The scheduler performs this check before the
/// allocation-free commit so environment or persisted-generation drift cannot
/// revoke earlier jobs after any result/source/lifecycle state is published.
/// @param[in] before Aggregate before the staged operation.
/// @param[in] staged Fully attested staged-operation delta.
/// @param[in] prospective Aggregate produced by merging `before` and `staged`.
/// @param[in] expected_jobs Exact job count attributed to `staged`.
/// @return `true` when staged and prospective stats preserve full attestation exactly.
[[nodiscard]] constexpr bool is_persisted_grouped_soa_coordinate_prospective_stats_consistent(
    const pipeline_execution_stats& before,
    const pipeline_execution_stats& staged,
    const pipeline_execution_stats& prospective,
    const std::uint64_t expected_jobs) noexcept {
    if (expected_jobs == 0 || expected_jobs > k_sql_bigint_value_max ||
        before.attempted_alignment_jobs > k_sql_bigint_value_max - expected_jobs) {
        return false;
    }
    const std::uint64_t expected_total = before.attempted_alignment_jobs + expected_jobs;
    return pipeline_execution_stats::can_retain_full_attestation(before) &&
           pipeline_execution_stats::can_retain_full_attestation(staged) &&
           pipeline_execution_stats::can_retain_full_attestation(prospective) &&
           staged.requested_backend == executed_backend::vulkan_gpu &&
           staged.actual_backend == executed_backend::vulkan_gpu &&
           staged.requested_target_index_source == target_index_source::persisted &&
           staged.actual_target_index_source == target_index_source::persisted &&
           staged.attempted_alignment_jobs == expected_jobs &&
           staged.fully_attested_alignment_jobs == expected_jobs &&
           staged.unattested_alignment_jobs == 0 &&
           prospective.attempted_alignment_jobs == expected_total &&
           prospective.fully_attested_alignment_jobs == expected_total &&
           prospective.unattested_alignment_jobs == 0 &&
           prospective.requested_backend == executed_backend::vulkan_gpu &&
           prospective.actual_backend == executed_backend::vulkan_gpu &&
           prospective.requested_target_index_source == target_index_source::persisted &&
           prospective.actual_target_index_source == target_index_source::persisted &&
           prospective.persisted_generation_id == staged.persisted_generation_id &&
           prospective.persisted_source_identity == staged.persisted_source_identity &&
           execution_environment_snapshot::is_payload_equal(prospective.environment,
                                                            staged.environment) &&
           !prospective.environment.is_mixed_invalid;
}

} // namespace bioseq::pipeline_stats
