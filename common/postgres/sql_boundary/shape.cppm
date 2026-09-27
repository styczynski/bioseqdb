/**
 * @file common/postgres/sql_boundary/shape.cppm
 * @brief C++ containers for rows returned by generated SQL functions.
 */
module;

#include <cstddef>
#include <type_traits>
#include <utility>
#include <vector>

export module bioseq.postgres.sql_boundary:shape;

export namespace bioseq::postgres::support {

/** The SQL result form selected by a generated function wrapper. */
enum class result_shape {
    set_of,            ///< A SETOF scalar result.
    table,             ///< A RETURNS TABLE row set.
    named_output_set,  ///< A SETOF result with named output columns.
};

/**
 * Own SQL result rows in a move-only container.
 *
 * @tparam p_element_type C++ value for one result row.
 * @tparam p_shape SQL result form used by the generated wrapper.
 */
template<class p_element_type, result_shape p_shape>
class result_rows final {
public:
    /** Value stored for each result row. */
    using value_type = p_element_type;
    /** Owning storage for the result rows. */
    using container_type = std::vector<p_element_type>;
    /** Mutable row iterator. */
    using iterator = typename container_type::iterator;
    /** Read-only row iterator. */
    using const_iterator = typename container_type::const_iterator;

    /** Take ownership of all rows. */
    explicit result_rows(container_type rows)
        noexcept(std::is_nothrow_move_constructible_v<container_type>)
        : rows_{std::move(rows)} {}

    result_rows(result_rows&&)
        noexcept(std::is_nothrow_move_constructible_v<container_type>) =
            default;
    result_rows& operator=(result_rows&&)
        noexcept(std::is_nothrow_move_assignable_v<container_type>) =
            default;
    result_rows(const result_rows&) = delete;
    result_rows& operator=(const result_rows&) = delete;

    /** Return an iterator to the first row. */
    [[nodiscard]] iterator begin() noexcept {
        return rows_.begin();
    }

    /** Return an iterator past the last row. */
    [[nodiscard]] iterator end() noexcept {
        return rows_.end();
    }

    /** Return a read-only iterator to the first row. */
    [[nodiscard]] const_iterator begin() const noexcept {
        return rows_.begin();
    }

    /** Return a read-only iterator past the last row. */
    [[nodiscard]] const_iterator end() const noexcept {
        return rows_.end();
    }

    /** Return the number of rows. */
    [[nodiscard]] std::size_t size() const noexcept {
        return rows_.size();
    }

    /** Return true when there are no rows. */
    [[nodiscard]] bool empty() const noexcept {
        return rows_.empty();
    }

private:
    container_type rows_;
};

/** Rows returned by a SETOF scalar routine. */
template<class p_element_type>
using set_of = result_rows<p_element_type, result_shape::set_of>;

/** Rows returned by a RETURNS TABLE routine. */
template<class p_row_type>
using table_rows = result_rows<p_row_type, result_shape::table>;

/** Rows returned by SETOF with named output columns. */
template<class p_row_type>
using named_output_set =
    result_rows<p_row_type, result_shape::named_output_set>;

/**
 * One composite value returned through named output parameters.
 *
 * @tparam p_row_type C++ value containing the named output fields.
 */
template<class p_row_type>
class named_output final {
public:
    /** Take ownership of the composite row value. */
    explicit named_output(p_row_type row)
        noexcept(std::is_nothrow_move_constructible_v<p_row_type>)
        : row_{std::move(row)} {}

    named_output(named_output&&)
        noexcept(std::is_nothrow_move_constructible_v<p_row_type>) = default;
    named_output& operator=(named_output&&)
        noexcept(std::is_nothrow_move_assignable_v<p_row_type>) = default;
    named_output(const named_output&) = delete;
    named_output& operator=(const named_output&) = delete;

    /** Return the mutable composite row. */
    [[nodiscard]] p_row_type& value() noexcept {
        return row_;
    }

    /** Return the read-only composite row. */
    [[nodiscard]] const p_row_type& value() const noexcept {
        return row_;
    }

private:
    p_row_type row_;
};

}  // namespace bioseq::postgres::support
