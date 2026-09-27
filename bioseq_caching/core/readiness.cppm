/**
 * @file bioseq_caching/core/readiness.cppm
 * @brief Compile-time correctness-readiness admission matrix.
 */
module;

#include <cstdint>

export module bioseq.cache.core:readiness;
import :wire;

export namespace bioseq::cache {

/** Reject unknown readiness values before ordinal comparisons. */
[[nodiscard]] constexpr bool known_readiness(
    cache_correctness_readiness readiness) noexcept {
    switch (readiness) {
        case cache_correctness_readiness::passthrough_only:
        case cache_correctness_readiness::relation_free_only:
        case cache_correctness_readiness::generic_mvcc_ready:
        case cache_correctness_readiness::provider_complete_ready:
        case cache_correctness_readiness::reusable_coverage_ready:
            return true;
    }
    return false;
}

/** Reject unknown observability values before readiness comparisons. */
[[nodiscard]] constexpr bool known_observability(
    write_observability observability) noexcept {
    switch (observability) {
        case write_observability::none:
        case write_observability::provider_complete:
        case write_observability::executor_utility_only:
        case write_observability::database_wide_fallback:
            return true;
    }
    return false;
}

/** Apply the normative readiness/kind/observability safety matrix. */
[[nodiscard]] constexpr bool readiness_allows(
    cache_correctness_readiness readiness,
    cache_entry_kind kind,
    write_observability observability,
    bool relation_free_builtin_safe,
    bool provider_backed) noexcept {
    if (!known_readiness(readiness) || !known_observability(observability)) {
        return false;
    }
    if (kind == cache_entry_kind::reusable_coverage) {
        return !relation_free_builtin_safe
            && provider_backed
            && readiness >= cache_correctness_readiness::reusable_coverage_ready
            && observability == write_observability::provider_complete;
    }
    if (kind != cache_entry_kind::exact_final_result) {
        return false;
    }
    if (provider_backed) {
        return !relation_free_builtin_safe
            && readiness >= cache_correctness_readiness::provider_complete_ready
            && observability == write_observability::provider_complete;
    }
    if (relation_free_builtin_safe) {
        return observability == write_observability::none
            && readiness >= cache_correctness_readiness::relation_free_only;
    }
    const bool conservative_generic =
        observability == write_observability::executor_utility_only
        || observability == write_observability::database_wide_fallback;
    return readiness >= cache_correctness_readiness::generic_mvcc_ready
        && conservative_generic;
}

} // namespace bioseq::cache
