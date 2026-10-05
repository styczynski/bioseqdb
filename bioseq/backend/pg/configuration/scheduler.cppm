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
    /// @brief Run dispatch and coarse pipeline work on the caller thread.
    single_thread = 0,
    /// @brief Allow independent batch items to run in the shared pool.
    parallel_dispatch = 1,
    /// @brief Each query dispatches separate worker (exxtension of parallel_dispatch) and
    /// additionally each stage of pipeline can submit independent tasks to TBB worker pool
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
         * at one  slot unless postgres_parallel.enabled is set.
         *
         * @remark Configuration: Limits maximum number of workers in TBB worker pool (this is separate to Postgres max workers setting)
         *
         * ```sql
         * -- Inspect the current value without changing the session.
         * SHOW bioseq.scheduler.backend.max_concurrency;
         * ```
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

        /** @brief Choose whether to parallelize dispatch, pipeline tasks or neither.
         *
         * @remark Configuration: Changes how the pipeline execution is performed In the benchmark I use task_parallel, because it enables the highest degree of parallelism
         * @remark Value `task_parallel`: Each query dispatches separate worker (exxtension of parallel_dispatch) and additionally each stage of pipeline can submit independent tasks to TBB worker pool
         * @remark Value `single_thread`: Run dispatch and coarse pipeline work on the caller thread.
         * @remark Value `parallel_dispatch`: Allow independent batch items to run in the shared pool.
         *
         * ```sql
         * -- Inspect the current value without changing the session.
         * SHOW bioseq.scheduler.pipeline.mode;
         * ```
         *
         * ```sql
         * -- Use the compiled default for one transaction; RESET removes the override and ROLLBACK restores the previous session state.
         * BEGIN;
         * SET LOCAL bioseq.scheduler.pipeline.mode = 'task_parallel';
         * SHOW bioseq.scheduler.pipeline.mode;
         * RESET bioseq.scheduler.pipeline.mode;
         * ROLLBACK;
         * ```
         */
        [[bioseq_sql::guc]]
        static inline bioseq::postgres::support::postgres_enum_storage<pipeline_concurrency_mode>
            mode = bioseq::postgres::support::postgres_enum_storage<pipeline_concurrency_mode>{
                pipeline_concurrency_mode::task_parallel};
    };

    /** Bioseq threads inside PostgreSQL parallel workers. */
    struct postgres_parallel final {
        postgres_parallel() = delete;

        /** @brief Let each PostgreSQL parallel worker use multiple Bioseq work slots.
         *
         * @remark Configuration: Reduce tasks slots to 1 This setting effectively forces bioseq.scheduler.backend.max_concurrency to be 1
         * @remark Value `false`: Disable any parallelism completely (note that if bioseq.scheduler.pipeline.mode is set to task_parallel, then pipeline will be executed via task queues, but only one worker will be active at the time. This setting is the highest level limiter on parallelism of workers)
         * @remark Value `true`: Allow >1 parallel tasks limit
         *
         * ```sql
         * -- Inspect the current value without changing the session.
         * SHOW bioseq.scheduler.postgres_parallel.enabled;
         * ```
         *
         * ```sql
         * -- Use the compiled default for one transaction; RESET removes the override and ROLLBACK restores the previous session state.
         * BEGIN;
         * SET LOCAL bioseq.scheduler.postgres_parallel.enabled = 'off';
         * SHOW bioseq.scheduler.postgres_parallel.enabled;
         * RESET bioseq.scheduler.postgres_parallel.enabled;
         * ROLLBACK;
         * ```
         */
        [[bioseq_sql::guc]]
        static inline bool enabled = false;
    };
};

} // namespace bioseq::scheduler
