module;

#include <cstdint>
#include <type_traits>

export module bioseq.sequences.sample_table_udf.sql_rows;

export namespace bioseq::sequences::generation::pg::result_rows {

struct locator_metadata final {
    std::int16_t version;
    std::int16_t bucket_idx;
    std::int32_t shard_idx;
    std::int64_t num_fragments;
    std::int64_t total_bp_size;
    std::int64_t first_sub_row_no;
    std::int64_t source_epoch;
    std::int32_t total_padding;
};

static_assert(std::is_aggregate_v<locator_metadata>);

}  // namespace bioseq::sequences::generation::pg::result_rows
