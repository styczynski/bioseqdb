/**
 * @file metadata_age_log_test_support_implementation.cppm
 * @brief Stress metadata-age updates using a long index name, then check row cleanup.
 */
module;

#include <bioseq/pg_compat.h>

#include <cstdint>
#include <stdexcept>
#include <string>

module bioseq.postgres.storage.indexing.metadata_age_log_test_support;

import bioseq.postgres.storage.indexing.chunked_storage;

namespace bioseq::storage::indexing {

void exercise_metadata_age_log_context_stress() {
    const std::string index_name(1024, 'm');
    for (std::uint32_t iteration = 0; iteration != 512; ++iteration) {
        static_cast<void>(upsert_metadata_age_log_pg(
            static_cast<Oid>(388079),
            static_cast<Oid>(388079),
            static_cast<std::int16_t>(1),
            index_name));
    }
    const std::uint64_t deleted = delete_metadata_age_log_pg(
        static_cast<Oid>(388079),
        static_cast<Oid>(388079),
        static_cast<std::int16_t>(1),
        index_name);
    if (deleted != 1) {
        throw std::runtime_error{
            "metadata_age_log stress row was not deleted"};
    }
}

} // namespace bioseq::storage::indexing
