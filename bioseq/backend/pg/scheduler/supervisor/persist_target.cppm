/**
 * @file persist_target.cppm
 * @brief Declares the target identity carried by asynchronous persistence jobs.
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
 * @brief Identifies one sequence index eligible for asynchronous persistence.
 *
 * The string view remains valid only while the owning backend-local job is alive.
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
