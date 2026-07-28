export module bioseq.scheduler.sql_guc_settings;

import bioseq.codegen.sql.annotation_model;
import bioseq.config.guc_storage;

export namespace bioseq::scheduler {

enum class PipelineConcurrencyMode : int {
    single = 0,
    parallel = 1,
    task_parallel = 2,
};

struct settings final {
    settings() = delete;

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

    /** @brief Selects how work is composed across pipeline tasks. */
    [[bioseq_sql::guc]]
    static inline
        bioseq::pg_utils::postgres_enum_storage<PipelineConcurrencyMode>
            pipeline_concurrency_mode =
                bioseq::pg_utils::postgres_enum_storage<
                    PipelineConcurrencyMode>{
                    PipelineConcurrencyMode::single};

    /** @brief Allows PostgreSQL parallel workers to use multiple task slots. */
    [[bioseq_sql::guc]]
    static inline bool allow_pg_parallel_composition = false;
};

}  // namespace bioseq::scheduler
