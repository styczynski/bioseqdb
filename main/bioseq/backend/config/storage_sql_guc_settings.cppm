module;

#include <climits>

export module bioseq.storage.sql_guc_settings;

import bioseq.codegen.sql.annotation_model;

export namespace bioseq::storage {

struct settings final {
    settings() = delete;

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
    static inline int max_seq_sub_table_gbs = 1'024;
};

}  // namespace bioseq::storage
