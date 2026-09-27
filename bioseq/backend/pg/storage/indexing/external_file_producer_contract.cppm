/**
 * @file external_file_producer_contract.cppm
 * @brief Zero counters reported for disabled external-file producer paths.
 */
module;

#include <cstdint>

export module bioseq.postgres.storage.indexing.external_file_producer_contract;

export namespace bioseq::storage::indexing {

/** Diagnostic fields for entries into external-file producer code. */
struct prohibited_external_file_producer_counters {
    std::uint64_t constructor_entries = 0;
    std::uint64_t writer_entries = 0;
};

/** Fixed zero values used by integration diagnostics while these producers are disabled. */
inline constexpr prohibited_external_file_producer_counters
    g_prohibited_external_file_producer_entry_counters{};
static_assert(g_prohibited_external_file_producer_entry_counters.constructor_entries == 0);
static_assert(g_prohibited_external_file_producer_entry_counters.writer_entries == 0);

} // namespace bioseq::storage::indexing
