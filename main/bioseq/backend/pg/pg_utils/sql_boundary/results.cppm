/*-------------------------------------------------------------------------
 *
 * pg_utils/sql_boundary/results.cppm
 *
 * Owning result-shape markers used by generated PostgreSQL boundaries.
 *
 *-------------------------------------------------------------------------
 */
module;

#include <cstddef>
#include <type_traits>
#include <utility>
#include <vector>

export module bioseq.pg_utils.sql_boundary:results;

export namespace bioseq::pg_utils {

enum class sql_result_kind {
    setof,
    returns_table,
    setof_named_out,
};

template<class T, sql_result_kind Kind>
class owning_sql_rows final {
public:
    using value_type = T;
    using container_type = std::vector<T>;
    using iterator = typename container_type::iterator;
    using const_iterator = typename container_type::const_iterator;

    explicit owning_sql_rows(container_type rows)
        noexcept(std::is_nothrow_move_constructible_v<container_type>)
        : rows_{std::move(rows)} {}

    owning_sql_rows(owning_sql_rows&&)
        noexcept(std::is_nothrow_move_constructible_v<container_type>) =
            default;
    owning_sql_rows& operator=(owning_sql_rows&&)
        noexcept(std::is_nothrow_move_assignable_v<container_type>) =
            default;
    owning_sql_rows(const owning_sql_rows&) = delete;
    owning_sql_rows& operator=(const owning_sql_rows&) = delete;

    [[nodiscard]] iterator begin() noexcept {
        return rows_.begin();
    }

    [[nodiscard]] iterator end() noexcept {
        return rows_.end();
    }

    [[nodiscard]] const_iterator begin() const noexcept {
        return rows_.begin();
    }

    [[nodiscard]] const_iterator end() const noexcept {
        return rows_.end();
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return rows_.size();
    }

    [[nodiscard]] bool empty() const noexcept {
        return rows_.empty();
    }

private:
    container_type rows_;
};

template<class T>
using setof = owning_sql_rows<T, sql_result_kind::setof>;

template<class Row>
using returns_table =
    owning_sql_rows<Row, sql_result_kind::returns_table>;

template<class Row>
using setof_named_out =
    owning_sql_rows<Row, sql_result_kind::setof_named_out>;

template<class Row>
class named_out final {
public:
    explicit named_out(Row row)
        noexcept(std::is_nothrow_move_constructible_v<Row>)
        : row_{std::move(row)} {}

    named_out(named_out&&)
        noexcept(std::is_nothrow_move_constructible_v<Row>) = default;
    named_out& operator=(named_out&&)
        noexcept(std::is_nothrow_move_assignable_v<Row>) = default;
    named_out(const named_out&) = delete;
    named_out& operator=(const named_out&) = delete;

    [[nodiscard]] Row& value() noexcept {
        return row_;
    }

    [[nodiscard]] const Row& value() const noexcept {
        return row_;
    }

private:
    Row row_;
};

}  // namespace bioseq::pg_utils

namespace bioseq::pg_utils::detail {

struct sql_result_probe_row {
    int value;
};

using setof_probe = setof<int>;
using table_probe = returns_table<sql_result_probe_row>;
using named_set_probe = setof_named_out<sql_result_probe_row>;
using named_probe = named_out<sql_result_probe_row>;

static_assert(std::is_same_v<
              typename setof_probe::value_type,
              int>);
static_assert(std::is_same_v<
              setof_probe,
              owning_sql_rows<int, sql_result_kind::setof>>);
static_assert(std::is_same_v<
              table_probe,
              owning_sql_rows<
                  sql_result_probe_row,
                  sql_result_kind::returns_table>>);
static_assert(std::is_same_v<
              named_set_probe,
              owning_sql_rows<
                  sql_result_probe_row,
                  sql_result_kind::setof_named_out>>);
static_assert(!std::is_default_constructible_v<setof_probe>);
static_assert(!std::is_copy_constructible_v<setof_probe>);
static_assert(!std::is_copy_assignable_v<setof_probe>);
static_assert(std::is_move_constructible_v<setof_probe>);
static_assert(std::is_move_assignable_v<setof_probe>);
static_assert(!std::is_copy_constructible_v<named_probe>);
static_assert(!std::is_copy_assignable_v<named_probe>);
static_assert(std::is_move_constructible_v<named_probe>);
static_assert(std::is_move_assignable_v<named_probe>);
static_assert(noexcept(std::declval<setof_probe&>().begin()));
static_assert(noexcept(std::declval<const setof_probe&>().begin()));
static_assert(noexcept(std::declval<const setof_probe&>().size()));
static_assert(noexcept(std::declval<const setof_probe&>().empty()));
static_assert(std::is_same_v<
              decltype(std::declval<named_probe&>().value()),
              sql_result_probe_row&>);
static_assert(std::is_same_v<
              decltype(std::declval<const named_probe&>().value()),
              const sql_result_probe_row&>);

}  // namespace bioseq::pg_utils::detail
