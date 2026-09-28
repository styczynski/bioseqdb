/**
 * @file common/postgres/sql_boundary/owning_values.cppm
 * @brief C++ values that keep their own copies of SQL argument and result data.
 *
 * Generated wrappers convert these values to and from PostgreSQL Datums.
 * Their strings, arrays and fixed-size fields can survive independently of
 * the memory holding the original SQL value.
 */
module;

#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

export module bioseq.postgres.sql_boundary:owning_values;

export namespace bioseq::postgres::support {

/** Own JSON text for a SQL JSONB value. PostgreSQL validates it when encoding. */
class jsonb_value final {
public:
    explicit jsonb_value(std::string encoded)
        : encoded_{std::move(encoded)} {}

    /** Return the stored JSON text. */
    [[nodiscard]] const std::string& encoded() const noexcept {
        return encoded_;
    }

private:
    std::string encoded_;
};

/** Own the bytes of a PostgreSQL BYTEA vaue */
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

/** Own the non-null integer elements of a PostgreSQL bigint array. */
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

/** Store PostgreSQL INTERVAL fields directly, with  no borrowed pointers. */
struct interval_value final {
    /** Signed time component in microseconds. */
    std::int64_t microseconds{};
    /** Signed calendar-day component. */
    std::int32_t days{};
    /** Signed calendar-month component. */
    std::int32_t months{};
};

/** Distinguish a finite timestamptz from negative or positive infinity. */
enum class timestamp_kind : std::uint8_t {
    finite,             ///< A finite PostgreSQL timestamp.
    negative_infinity,  ///< PostgreSQL's negative-infinity sentinel.
    positive_infinity,  ///< PostgreSQL's positive-infinity sentinel.
};

/**
 * A PostgreSQL timestamptz stored as a C++ value.
 * Finite values count signed microseconds from 2000-01-01. For either infinity
 * kind, the converter ignores postgres_epoch_microseconds.
 */
struct timestamp_with_time_zone final {
    /** Whether this value is finite, negative infinity or positive infinity. */
    timestamp_kind kind{timestamp_kind::finite};
    /** Signed microseconds from PostgreSQL's 2000-01-01 epoch. */
    std::int64_t postgres_epoch_microseconds{};
};

/** A physical row address stored as a block number and item offset. */
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
