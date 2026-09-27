/**
 * @file storage.cppm
 * @brief PostgreSQL settings for splitting sequence storage into shards.
 */
module;

#include <limits>

export module bioseq.postgres.configuration.storage;

import bioseq.codegen.sql.annotation_model;

export namespace bioseq::storage {

/** PostgreSQL settings for stored sequence data. */
struct settings final {
    settings() = delete;

    /** Size limit for a sequence-storage shard. */
    struct sequence_shard final {
        sequence_shard() = delete;

        /**
         * @brief Limits one sequence storage shard in gibibytes.
         *
         * Zero starts a new shard for every inserted sequence.
         */
        [[bioseq_sql::guc({
            .context = bioseq::sql::guc_context::superuser,
            .minimum = 0,
            .maximum = std::numeric_limits<int>::max(),
        })]]
        static inline int max_size_gib = 1'024;
    };
};

} // namespace bioseq::storage
