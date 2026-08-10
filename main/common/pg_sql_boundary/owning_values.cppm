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

export namespace bioseq::postgres::support {

/** Owning JSONB text in PostgreSQL's canonical output representation. */
class jsonb_value final {
public:
    explicit jsonb_value(std::string encoded)
        : encoded_{std::move(encoded)} {}

    /** Return the canonical JSONB text. */
    [[nodiscard]] const std::string& encoded() const noexcept {
        return encoded_;
    }

private:
    std::string encoded_;
};

/** Owning PostgreSQL BYTEA payload. */
class bytea_value final {
public:
    explicit bytea_value(std::vector<std::byte> bytes)
        : bytes_{std::move(bytes)} {}

    /** Return the owned byte payload. */
    [[nodiscard]] const std::vector<std::byte>& bytes() const noexcept {
        return bytes_;
    }

private:
    std::vector<std::byte> bytes_;
};

/** Owning values for a PostgreSQL bigint array. */
class bigint_array final {
public:
    explicit bigint_array(std::vector<std::int64_t> values)
        : values_{std::move(values)} {}

    /** Return the owned array elements. */
    [[nodiscard]] const std::vector<std::int64_t>&
    values() const noexcept {
        return values_;
    }

private:
    std::vector<std::int64_t> values_;
};

/** Owning PostgreSQL INTERVAL fields without backend-owned pointers. */
struct interval_value final {
    /** Signed time component in microseconds. */
    std::int64_t microseconds{};
    /** Signed calendar-day component. */
    std::int32_t days{};
    /** Signed calendar-month component. */
    std::int32_t months{};
};

/** Classification for one owning PostgreSQL timestamptz value. */
enum class timestamp_kind : std::uint8_t {
    finite,             ///< A finite PostgreSQL timestamp.
    negative_infinity,  ///< PostgreSQL's negative-infinity sentinel.
    positive_infinity,  ///< PostgreSQL's positive-infinity sentinel.
};

/**
 * Owning representation of PostgreSQL timestamptz.
 *
 * Finite values are signed microseconds from PostgreSQL's 2000-01-01 epoch.
 * `postgres_epoch_microseconds` is ignored for either infinity kind.
 */
struct timestamp_with_time_zone final {
    /** Finite or infinite classification. */
    timestamp_kind kind{timestamp_kind::finite};
    /** Signed microseconds from PostgreSQL's 2000-01-01 epoch. */
    std::int64_t postgres_epoch_microseconds{};
};

/** Owning physical tuple identifier. */
struct tuple_identifier final {
    /** Heap block number. */
    std::uint32_t block_number{};
    /** One-based item offset within the heap block. */
    std::uint16_t offset_number{};
};

}  // namespace bioseq::postgres::support

namespace bioseq::postgres::support::detail {

static_assert(std::is_trivially_copyable_v<timestamp_with_time_zone>);
static_assert(std::is_trivially_destructible_v<timestamp_with_time_zone>);
static_assert(
    std::is_same_v<
        std::underlying_type_t<timestamp_kind>,
        std::uint8_t>);

}  // namespace bioseq::postgres::support::detail
