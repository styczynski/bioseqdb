/**
 * @file scanner/source_test_contract.cppm
 * @brief Inject read failures and count source and output cleanup in tests.
 */
module;

extern "C" {
#include "postgres.h"
#include "utils/errcodes.h"
}

#undef Min
#undef Max
#undef snprintf
#undef vsnprintf

#include <cstdint>

export module bioseq.postgres.storage.scanner.source_test_contract;

export namespace bioseq::storage {

#ifdef BIOSEQ_TEST_INDEXES
/// Places where tests can raise a one-time source or output error.
enum class sequence_source_failure : std::uint8_t {
    none,
    provider_after_registry_control,
    provider_after_provider_control,
    domain_after_control,
    domain_after_context,
    domain_after_owner,
    domain_after_snapshot,
    domain_after_caches,
    close_before_caches,
    close_before_snapshot,
    close_before_before_locks,
    close_before_locks,
    close_before_after_locks,
    read_before_scanner,
    mmsearch_output_after_promotion,
};

/// Test counts for source creation, reads and cleanup, including live resources.
struct sequence_source_stats {
    std::uint64_t registries_created = 0u;
    std::uint64_t registries_destroyed = 0u;
    std::uint64_t providers_created = 0u;
    std::uint64_t providers_destroyed = 0u;
    std::uint64_t scan_providers_created = 0u;
    std::uint64_t standalone_providers_created = 0u;
    std::uint64_t domains_created = 0u;
    std::uint64_t domains_reused = 0u;
    std::uint64_t domains_destroyed = 0u;
    std::uint64_t cache_sets_created = 0u;
    std::uint64_t cache_sets_destroyed = 0u;
    std::uint64_t snapshots_registered = 0u;
    std::uint64_t snapshots_unregistered = 0u;
    std::uint64_t scanner_entries_created = 0u;
    std::uint64_t scanner_entry_hits = 0u;
    std::uint64_t leases_acquired = 0u;
    std::uint64_t leases_released = 0u;
    std::uint64_t provider_closed_with_upper_leases = 0u;
    std::uint64_t last_upper_lease_after_provider_end = 0u;
    std::uint64_t cleanup_commit = 0u;
    std::uint64_t cleanup_abort = 0u;
    std::uint64_t sweeps = 0u;
    std::uint64_t swept_domains = 0u;
    std::uint64_t subxact_promotions = 0u;
    std::uint64_t subxact_aborts = 0u;
    std::uint64_t pre_prepare_rejections = 0u;
    std::uint64_t release_before_locks = 0u;
    std::uint64_t release_locks = 0u;
    std::uint64_t release_after_locks = 0u;
    std::uint64_t wrong_thread_rejections = 0u;
    std::uint64_t refcount_faults = 0u;
    std::uint64_t lease_faults = 0u;
    std::uint64_t fault_injections = 0u;
    std::uint64_t read_fault_injections = 0u;
    std::uint64_t mmsearch_output_context_resets = 0u;
    std::uint64_t mmsearch_output_locator_promotions = 0u;
    std::uint64_t mmsearch_output_pg_error_restores = 0u;
    std::uint64_t mmsearch_output_restore_mismatches = 0u;
    std::uint64_t facade_source_stream_callbacks = 0u;
    std::uint64_t facade_source_resident_probes = 0u;
    std::uint64_t facade_declared_materializations = 0u;
    std::uint64_t facade_completed_materializations = 0u;
    std::uint64_t facade_poison_hits = 0u;
    std::uint64_t live_registries = 0u;
    std::uint64_t live_providers = 0u;
    std::uint64_t live_domains = 0u;
    std::uint64_t live_leases = 0u;
    std::uint64_t live_cache_sets = 0u;
    std::uint64_t live_snapshots = 0u;
};

inline thread_local sequence_source_failure g_pg_sequence_source_failure =
    sequence_source_failure::none;
inline thread_local sequence_source_stats g_pg_sequence_source_stats{};

/// Raise a test error on the next visit to the selected location.
/// @param point Failure location, or `none` to disable the error.
inline void
arm_pg_sequence_source_failure(sequence_source_failure point) noexcept {
    g_pg_sequence_source_failure = point;
}

namespace sequence_source_detail {

/// Raise  and clear the test errnr if this is its selected location
/// @throws PostgreSQL ERROR when `point` is armed.
/// @param point Failure location, being entered.
inline void inject_pg_sequence_source_failure(sequence_source_failure point) {
    if (g_pg_sequence_source_failure != point) {
        return;
    }
    g_pg_sequence_source_failure = sequence_source_failure::none;
    ++g_pg_sequence_source_stats.fault_injections;
    if (point == sequence_source_failure::read_before_scanner) {
        ++g_pg_sequence_source_stats.read_fault_injections;
    }
    ereport(ERROR,
        (errcode(ERRCODE_INTERNAL_ERROR),
         errmsg("injected PostgreSQL sequence source-domain failure"),
         errdetail("failure point %u", static_cast<unsigned>(point))));
}

} // namespace sequence_source_detail
#endif

/* Output cleanup uses the source test counters. These functions do
 * nothing in production builds. */
/// Record an output memory-context reset in test builds.
inline void note_mmsearch_output_context_reset_for_test() noexcept {
#ifdef BIOSEQ_TEST_INDEXES
    ++g_pg_sequence_source_stats
        .mmsearch_output_context_resets;
#endif
}

/// Count an output locator converted to an expanded sequence in test builds.
/// @param promoted_locator Whether the output locator was opened.
inline void note_mmsearch_output_locator_promotion_for_test(
        const bool promoted_locator) noexcept {
#ifdef BIOSEQ_TEST_INDEXES
    if (promoted_locator) {
        ++g_pg_sequence_source_stats
            .mmsearch_output_locator_promotions;
    }
#else
    (void)promoted_locator;
#endif
}

/// Count error recovery in test builds and record whether prior state was restored.
/// @param state_restored Whether the output  owner returned to its prior state.
inline void note_mmsearch_output_pg_error_restore_for_test(
        const bool state_restored) noexcept {
#ifdef BIOSEQ_TEST_INDEXES
    ++g_pg_sequence_source_stats
        .mmsearch_output_pg_error_restores;
    if (!state_restored) {
        ++g_pg_sequence_source_stats
            .mmsearch_output_restore_mismatches;
    }
#else
    (void)state_restored;
#endif
}

/// Raise the selected test error immediately after an output locator is opened.
/// @throws PostgreSQL ERROR when this test error is armed.
inline void inject_mmsearch_output_after_promotion_failure_for_test() {
#ifdef BIOSEQ_TEST_INDEXES
    sequence_source_detail::inject_pg_sequence_source_failure(
        sequence_source_failure::mmsearch_output_after_promotion);
#endif
}

} // namespace bioseq::storage
