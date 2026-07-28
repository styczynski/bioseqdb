import bioseq.codegen.sql.annotation_probe;
import bioseq.codegen.sql.annotation_model;

namespace bioseq::codegen::sql::ordinary_annotation_probe {

struct [[bioseq_sql::table({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
})]] row final {
    [[bioseq_sql::column({})]]
    int value;
};

[[bioseq_sql::function({
    .volatility = bioseq::sql::volatility::immutable,
})]]
[[bioseq_sql::operator_({.symbol = ">"})]]
inline bool greater(
    [[bioseq_sql::param({.sql_type = bioseq::sql::pg::integer})]]
    int left,
    [[bioseq_sql::param({.sql_type = bioseq::sql::pg::integer})]]
    int right) {
    return left > right;
}

}  // namespace bioseq::codegen::sql::ordinary_annotation_probe

static_assert(
    bioseq::codegen::sql::annotation_probe::identity(7) == 7);
static_assert(
    static_cast<int>(
        bioseq::codegen::sql::annotation_probe::planner_level::info)
    == 17);
