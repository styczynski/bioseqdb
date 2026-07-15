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
    CacheCorrectnessReadiness readiness) noexcept {
    switch (readiness) {
        case CacheCorrectnessReadiness::PassthroughOnly:
        case CacheCorrectnessReadiness::RelationFreeOnly:
        case CacheCorrectnessReadiness::GenericMvccReady:
        case CacheCorrectnessReadiness::ProviderCompleteReady:
        case CacheCorrectnessReadiness::ReusableCoverageReady:
            return true;
    }
    return false;
}

/** Reject unknown observability values before readiness comparisons. */
[[nodiscard]] constexpr bool known_observability(
    WriteObservability observability) noexcept {
    switch (observability) {
        case WriteObservability::None:
        case WriteObservability::ProviderComplete:
        case WriteObservability::ExecutorUtilityOnly:
        case WriteObservability::DatabaseWideFallback:
            return true;
    }
    return false;
}

/** Apply the normative readiness/kind/observability safety matrix. */
[[nodiscard]] constexpr bool readiness_allows(
    CacheCorrectnessReadiness readiness,
    CacheEntryKind kind,
    WriteObservability observability,
    bool relation_free_builtin_safe,
    bool provider_backed) noexcept {
    if (!known_readiness(readiness) || !known_observability(observability)) {
        return false;
    }
    if (kind == CacheEntryKind::ReusableCoverage) {
        return !relation_free_builtin_safe
            && provider_backed
            && readiness >= CacheCorrectnessReadiness::ReusableCoverageReady
            && observability == WriteObservability::ProviderComplete;
    }
    if (kind != CacheEntryKind::ExactFinalResult) {
        return false;
    }
    if (provider_backed) {
        return !relation_free_builtin_safe
            && readiness >= CacheCorrectnessReadiness::ProviderCompleteReady
            && observability == WriteObservability::ProviderComplete;
    }
    if (relation_free_builtin_safe) {
        return observability == WriteObservability::None
            && readiness >= CacheCorrectnessReadiness::RelationFreeOnly;
    }
    const bool conservative_generic =
        observability == WriteObservability::ExecutorUtilityOnly
        || observability == WriteObservability::DatabaseWideFallback;
    return readiness >= CacheCorrectnessReadiness::GenericMvccReady
        && conservative_generic;
}

} // namespace bioseq::cache
