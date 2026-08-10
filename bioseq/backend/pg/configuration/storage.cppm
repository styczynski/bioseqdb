module;

#include <climits>

export module bioseq.postgres.configuration.storage;

import bioseq.codegen.sql.annotation_model;

export namespace bioseq::storage {

/** Owns PostgreSQL settings for Bioseq sequence storage. */
struct settings final {
    settings() = delete;

    /** Owns settings that bound one sequence shard. */
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
            .maximum = INT_MAX,
        })]]
        static inline int max_size_gib = 1'024;
    };
};

} // namespace bioseq::storage
