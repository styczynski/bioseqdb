/**
 * @file sql_boundary_template_producer_test.cppm
 * @brief Instantiates generated SQL-boundary templates for compile checks.
 */
module;

#include <bioseq/pg_compat.h>

#include <cstdint>
#include <utility>
#include <vector>

export module bioseq.postgres.sql.generated.core.sql_boundary_template_producer_test;

export import bioseq.postgres.support.sql_boundary;
export import bioseq.postgres.storage.indexing.sql_row_codecs;
export import bioseq.postgres.storage.manager.sql_row_codecs;
export import bioseq.postgres.storage.metadata.sql_row_codecs;

export namespace bioseq::sql::generated::core::
    sql_boundary_template_producer_test {

/** @brief Minimal row used to instantiate table and named result wrappers. */
struct result_row final {
    std::int32_t value;
};

using setof_result = bioseq::postgres::support::set_of<std::int32_t>;
using table_result = bioseq::postgres::support::table_rows<result_row>;
using named_set_result =
    bioseq::postgres::support::named_output_set<result_row>;
using named_result = bioseq::postgres::support::named_output<result_row>;

using chunk_group_codec =
    bioseq::postgres::storage::metadata::sql_row_codecs::chunk_group_codec;
using chunk_codec =
    bioseq::postgres::storage::metadata::sql_row_codecs::chunk_codec;
using options_codec =
    bioseq::postgres::storage::metadata::sql_row_codecs::options_codec;
using row_mask_codec =
    bioseq::postgres::storage::metadata::sql_row_codecs::row_mask_codec;
using index_policy_codec =
    bioseq::postgres::storage::indexing::sql_row_codecs::
        index_policy_codec;
using metadata_age_log_codec =
    bioseq::postgres::storage::indexing::sql_row_codecs::
        metadata_age_log_codec;
using minimizer_generation_storage_codec =
    bioseq::postgres::storage::manager::sql_row_codecs::
        minimizer_generation_storage_codec;

/** @brief Minimal PostgreSQL boundary callable used for template instantiation. */
struct boundary_callable final {
    [[nodiscard]] Datum operator()() const noexcept {
        return static_cast<Datum>(0);
    }
};

using boundary_signature = Datum (*)(boundary_callable&&);

/** @brief Forces instantiation of the PostgreSQL callback adapter. */
inline constexpr boundary_signature boundary_instantiation =
    &bioseq::postgres::support::invoke_postgres_callback<boundary_callable>;

/** @return A representative scalar set result. */
[[nodiscard]] inline setof_result make_setof_result() {
    return setof_result{
        std::vector<std::int32_t>{1, 2}};
}

/** @return A representative table result. */
[[nodiscard]] inline table_result make_table_result() {
    return table_result{
        std::vector<result_row>{{.value = 3}}};
}

/** @return A representative named set result. */
[[nodiscard]] inline named_set_result make_named_set_result() {
    return named_set_result{
        std::vector<result_row>{{.value = 4}}};
}

/** @return A representative named scalar result. */
[[nodiscard]] inline named_result make_named_result() {
    return named_result{result_row{.value = 5}};
}

}  // namespace bioseq::sql::generated::core::
   // sql_boundary_template_producer_test
