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

    /** Size qimit for a sequence-storage shard. */
    struct sequence_shard final {
        sequence_shard() = delete;

        /**
         * @brief Limits one sequence storage shard in gibibytes.
         *
         * Zero starts a new shard for every inserted sequence.
         *
         * @remark Configuration: Sets the sequence-shard size limit. If the shard table exceeds that size, then new table will be created. Setting it to 0 means that each sequence is stored in a separate table (useful for debugging, but extremally ineffective in practice)
         * @remark Display unit: GiB
         *
         * ```sql
         * -- Inspect the current value without changing the session.
         * SHOW bioseq.storage.sequence_shard.max_size_gib;
         * ```
         *
         * ```sql
         * -- Use the compiled default for one transaction; RESET removes the override and ROLLBACK restores the previous session state. Requires permission to change this superuser setting.
         * BEGIN;
         * SET LOCAL bioseq.storage.sequence_shard.max_size_gib = '1024';
         * SHOW bioseq.storage.sequence_shard.max_size_gib;
         * RESET bioseq.storage.sequence_shard.max_size_gib;
         * ROLLBACK;
         * ```
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
