/**
 * @file persist_target.cppm
 * @brief Identify the table, column, and equence for a queued index build.
 */
module;

extern "C" {
#include "postgres.h"
}

#undef Min
#undef Max
#undef snprintf
#undef vsnprintf

#include <cstdint>
#include <string_view>

export module bioseq.postgres.scheduler.persist_target;

export namespace bioseq::postgres::scheduler {

/**
 * Identify, the sequence  index requested by a background build.
 * index_name borrows its string. Keep the string owner alive while using this
 * target, or copy the name when transferring it to a pending job.
 */
struct persist_target {
    Oid db_oid = InvalidOid;
    Oid user_relid = InvalidOid;
    Oid tablespace_oid = InvalidOid;
    Oid main_relfilenode = InvalidOid;
    std::int16_t column_attno = 0;
    std::uint8_t bucket_idx = 0;
    std::uint32_t shard_idx = 0;
    std::uint64_t sequence_rowno = 0;
    std::string_view index_name{};
};

} // namespace bioseq::postgres::scheduler
