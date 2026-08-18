/**
 * @file external_file_producer_contract.cppm
 * @brief Exposes the compile-disabled external-file producer counters.
 */
export module bioseq.postgres.storage.indexing.external_file_producer_contract;

import std;

export namespace bioseq::storage::indexing {

/** Counts entries into the disabled external-file producer paths. */
struct prohibited_external_file_producer_counters {
    std::uint64_t constructor_entries = 0;
    std::uint64_t writer_entries = 0;
};

/** Immutable zero counters exposed to integration diagnostics. */
inline constexpr prohibited_external_file_producer_counters
    g_prohibited_external_file_producer_entry_counters{};
static_assert(g_prohibited_external_file_producer_entry_counters.constructor_entries == 0);
static_assert(g_prohibited_external_file_producer_entry_counters.writer_entries == 0);

} // namespace bioseq::storage::indexing
