/**
 * @file scheduler.cppm
 * @brief Declares PostgreSQL settings for scheduler execution.
 */
export module bioseq.postgres.configuration.scheduler;

import bioseq.codegen.sql.annotation_model;
import bioseq.postgres.support.enum_storage;

export namespace bioseq::scheduler {

/** Selects how scheduler work is composed across pipeline tasks. */
enum class pipeline_concurrency_mode : int {
    single_thread = 0,
    parallel_dispatch = 1,
    task_parallel = 2,
};

/** Owns PostgreSQL settings for scheduler execution. */
struct settings final {
    settings() = delete;

    /** Owns per-backend scheduler capacity. */
    struct backend final {
        backend() = delete;

        /**
         * @brief Limits worker slots used by each backend.
         *
         * Zero selects the available CPU count.
         */
        [[bioseq_sql::guc({
            .context = bioseq::sql::guc_context::sighup,
            .minimum = 0,
            .maximum = 1'024,
        })]]
        static inline int max_concurrency = 0;
    };

    /** Owns pipeline work-composition settings. */
    struct pipeline final {
        pipeline() = delete;

        /** @brief Selects how work is composed across pipeline tasks. */
        [[bioseq_sql::guc]]
        static inline bioseq::postgres::support::postgres_enum_storage<pipeline_concurrency_mode>
            mode = bioseq::postgres::support::postgres_enum_storage<pipeline_concurrency_mode>{
                pipeline_concurrency_mode::task_parallel};
    };

    /** Owns PostgreSQL parallel-worker composition settings. */
    struct postgres_parallel final {
        postgres_parallel() = delete;

        /** @brief Allows PostgreSQL parallel workers to use multiple task slots. */
        [[bioseq_sql::guc]]
        static inline bool enabled = false;
    };
};

} // namespace bioseq::scheduler
