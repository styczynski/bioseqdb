module;

#include <cstdint>

export module bioseq.codegen.sql.annotation_probe;

import bioseq.codegen.sql.annotation_model;

export namespace bioseq::codegen::sql::annotation_probe {

inline constexpr bioseq::sql::column_options reusable_column_options{
    .sql_type = bioseq::sql::pg::bigint,
};

struct [[bioseq_sql::type({
    .internal_length = 2,
    .passed_by_value = true,
    .alignment = bioseq::sql::type_alignment::int2,
    .storage = bioseq::sql::type_storage::plain,
})]] letter {
    [[bioseq_sql::column({.sql_type = bioseq::sql::pg::smallint})]]
    std::int16_t rank{};

    [[bioseq_sql::function({
        .volatility = bioseq::sql::volatility::immutable,
        .parallel = bioseq::sql::parallel_safety::safe,
    })]]
    [[bioseq_sql::operator_({.symbol = "="})]]
    static bool letter_eq(letter left, letter right) {
        return left.rank == right.rank;
    }
};

using sequence_alias [[bioseq_sql::type]] = std::int64_t;

struct [[bioseq_sql::table({
            .schema =
                bioseq::sql::fixed_schema<"bioseq_internal">,
            .create = bioseq::sql::create_mode::if_not_exists,
        }),
        bioseq_sql::primary_key({
            .columns = {"storage_id", "stripe_num"},
        })]] row_probe {
    [[bioseq_sql::column({})]]
    std::int64_t storage_id;

    [[bioseq_sql::column(reusable_column_options)]]
    std::int64_t stripe_num;
};

enum class planner_level : int {
    debug2 [[bioseq_sql::guc_value({
        .aliases = bioseq::sql::guc_aliases({
            .spelling = "debug",
            .hidden = true,
        }),
    })]] = 13,
    info = 17,
};

[[bioseq_sql::function({
    .volatility = bioseq::sql::volatility::immutable,
})]]
inline constexpr std::int32_t identity(
    [[bioseq_sql::param({.sql_type = bioseq::sql::pg::integer})]]
    std::int32_t value) {
    return value;
}

[[bioseq_sql::function({
    .volatility = bioseq::sql::volatility::immutable,
})]]
[[bioseq_sql::operator_({.symbol = "<"})]]
inline constexpr bool rank_less(letter left, letter right) {
    return left.rank < right.rank;
}

struct settings_probe final {
    settings_probe() = delete;

    [[bioseq_sql::guc({
        .context = bioseq::sql::guc_context::user,
    })]]
    static inline bool enabled = true;
};

static_assert(
    bioseq::sql::type_of<letter>.id
    == bioseq::sql::pg_type_id::custom);

[[bioseq_sql::sequence({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
})]]
inline constexpr std::int64_t sequence_probe = 1;

[[bioseq_sql::schema]]
inline constexpr bioseq::sql::schema_entity schema_probe{};

}  // namespace bioseq::codegen::sql::annotation_probe
