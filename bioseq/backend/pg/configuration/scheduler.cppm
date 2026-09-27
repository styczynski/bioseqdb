/**
 * @file scheduler.cppm
 * @brief PostgreSQL settings for Bioseq worker threads.
 */
export module bioseq.postgres.configuration.scheduler;

import bioseq.codegen.sql.annotation_model;
import bioseq.postgres.support.enum_storage;

export namespace bioseq::scheduler {

/** Choose caller-only execution, parallel dispatch or parallel pipeline tasks. */
enum class pipeline_concurrency_mode : int {
    single_thread = 0,
    parallel_dispatch = 1,
    task_parallel = 2,
};

/** PostgreSQL settings for the Bioseq scheduler. */
struct settings final {
    settings() = delete;

    /** Worker capacity within one PostgreSQL backend. */
    struct backend final {
        backend() = delete;

        /**
         * @brief Maximum concurrent work slots in one PostgreSQL backend.
         * Zero uses the available CPU count. PostgreSQL parallel workers stay
         * at one slot unless postgres_parallel.enabled is set.
         */
        [[bioseq_sql::guc({
            .context = bioseq::sql::guc_context::sighup,
            .minimum = 0,
            .maximum = 1'024,
        })]]
        static inline int max_concurrency = 0;
    };

    /** Which parts of a pipeline may use worker threads. */
    struct pipeline final {
        pipeline() = delete;

        /** @brief Choose whether to parallelize dispatch, pipeline tasks or neither. */
        [[bioseq_sql::guc]]
        static inline bioseq::postgres::support::postgres_enum_storage<pipeline_concurrency_mode>
            mode = bioseq::postgres::support::postgres_enum_storage<pipeline_concurrency_mode>{
                pipeline_concurrency_mode::task_parallel};
    };

    /** Bioseq threads inside PostgreSQL parallel workers. */
    struct postgres_parallel final {
        postgres_parallel() = delete;

        /** @brief Let each PostgreSQL parallel worker use multiple Bioseq work slots. */
        [[bioseq_sql::guc]]
        static inline bool enabled = false;
    };
};

} // namespace bioseq::scheduler
