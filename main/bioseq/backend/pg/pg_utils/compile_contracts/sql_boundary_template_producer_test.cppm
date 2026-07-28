module;

#include "common.h"

#include <cstdint>
#include <utility>
#include <vector>

export module bioseq.sql.generated.core.sql_boundary_template_producer_test;

export import bioseq.pg_utils.sql_boundary;
export import bioseq.storage.indexing_pg.sql_row_codecs;
export import bioseq.storage.manager.sql_row_codecs;
export import bioseq.storage.metadata.sql_row_codecs;

export namespace bioseq::sql::generated::core::
    sql_boundary_template_producer_test {

struct result_row final {
    std::int32_t value;
};

using setof_result = bioseq::pg_utils::setof<std::int32_t>;
using table_result = bioseq::pg_utils::returns_table<result_row>;
using named_set_result =
    bioseq::pg_utils::setof_named_out<result_row>;
using named_result = bioseq::pg_utils::named_out<result_row>;

using chunk_group_codec =
    bioseq::storage::metadata::sql_row_codecs::chunk_group_codec;
using chunk_codec =
    bioseq::storage::metadata::sql_row_codecs::chunk_codec;
using options_codec =
    bioseq::storage::metadata::sql_row_codecs::options_codec;
using row_mask_codec =
    bioseq::storage::metadata::sql_row_codecs::row_mask_codec;
using index_policy_codec =
    bioseq::storage::indexing_pg::sql_row_codecs::
        index_policy_codec;
using metadata_age_log_codec =
    bioseq::storage::indexing_pg::sql_row_codecs::
        metadata_age_log_codec;
using minimizer_generation_storage_codec =
    bioseq::storage::manager::sql_row_codecs::
        minimizer_generation_storage_codec;

struct boundary_callable final {
    [[nodiscard]] Datum operator()() const noexcept {
        return static_cast<Datum>(0);
    }
};

using boundary_signature = Datum (*)(
    bioseq::pg_utils::sql_boundary_probe_descriptor,
    boundary_callable&&);

inline constexpr boundary_signature boundary_instantiation =
    &bioseq::pg_utils::invoke_sql_boundary<boundary_callable>;

[[nodiscard]] inline setof_result make_setof_result() {
    return setof_result{
        std::vector<std::int32_t>{1, 2}};
}

[[nodiscard]] inline table_result make_table_result() {
    return table_result{
        std::vector<result_row>{{.value = 3}}};
}

[[nodiscard]] inline named_set_result make_named_set_result() {
    return named_set_result{
        std::vector<result_row>{{.value = 4}}};
}

[[nodiscard]] inline named_result make_named_result() {
    return named_result{result_row{.value = 5}};
}

}  // namespace bioseq::sql::generated::core::
   // sql_boundary_template_producer_test
