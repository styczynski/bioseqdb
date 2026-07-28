/*-------------------------------------------------------------------------
 *
 * pg_utils/sql_boundary/values.cppm
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
#include <utility>
#include <vector>

export module bioseq.pg_utils.sql_boundary:values;

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

struct sql_interval final {
    std::int64_t time{};
    std::int32_t day{};
    std::int32_t month{};
};

struct sql_tid final {
    std::uint32_t block_number{};
    std::uint16_t offset_number{};
};

}  // namespace bioseq::pg_utils
