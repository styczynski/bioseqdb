/*-------------------------------------------------------------------------
 *
 * common/pg_sql_boundary/owning_values.cppm
 *
 * Owning C++ values for PostgreSQL varlena and pass-by-reference boundary
 * types.  The generated fmgr layer converts these values at the outermost
 * PostgreSQL boundary so C++ routine owners never retain borrowed Datums.
 *
 *-------------------------------------------------------------------------
 */
module;

#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

export module bioseq.pg_sql_boundary:owning_values;

export namespace bioseq::pg_utils {

class sql_jsonb final {
public:
    explicit sql_jsonb(std::string encoded)
        : encoded_{std::move(encoded)} {}

    [[nodiscard]] const std::string& encoded() const noexcept {
        return encoded_;
    }

private:
    std::string encoded_;
};

class sql_bytea final {
public:
    explicit sql_bytea(std::vector<std::byte> bytes)
        : bytes_{std::move(bytes)} {}

    [[nodiscard]] const std::vector<std::byte>& bytes() const noexcept {
        return bytes_;
    }

private:
    std::vector<std::byte> bytes_;
};

/** Owning values for a PostgreSQL bigint[] result. */
class sql_int64_array final {
public:
    explicit sql_int64_array(std::vector<std::int64_t> values)
        : values_{std::move(values)} {}

    [[nodiscard]] const std::vector<std::int64_t>&
    values() const noexcept {
        return values_;
    }

private:
    std::vector<std::int64_t> values_;
};

struct sql_interval final {
    std::int64_t time{};
    std::int32_t day{};
    std::int32_t month{};
};

/** Classification for one owning PostgreSQL timestamptz value. */
enum class sql_timestamp_kind : std::uint8_t {
    finite,
    negative_infinity,
    positive_infinity,
};

/**
 * Owning representation of PostgreSQL timestamptz.
 *
 * Finite values are signed microseconds from PostgreSQL's 2000-01-01 epoch.
 * `postgres_microseconds` is ignored for either infinity kind.
 */
struct sql_timestamptz final {
    sql_timestamp_kind kind{sql_timestamp_kind::finite};
    std::int64_t postgres_microseconds{};
};

struct sql_tid final {
    std::uint32_t block_number{};
    std::uint16_t offset_number{};
};

}  // namespace bioseq::pg_utils

namespace bioseq::pg_utils::detail {

static_assert(std::is_trivially_copyable_v<sql_timestamptz>);
static_assert(std::is_trivially_destructible_v<sql_timestamptz>);
static_assert(
    std::is_same_v<
        std::underlying_type_t<sql_timestamp_kind>,
        std::uint8_t>);

}  // namespace bioseq::pg_utils::detail
