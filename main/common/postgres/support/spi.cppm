/**
 * @file common/postgres/support/spi.cppm
 * @brief Scoped PostgreSQL Server Programming Interface connection owner.
 */
module;

extern "C" {
#include "postgres.h"

#include "executor/spi.h"
#include "utils/errcodes.h"
}

#include <type_traits>

export module bioseq.postgres.support:spi;

import :error_boundary;

export namespace bioseq::postgres::support {

/**
 * Owns one nested PostgreSQL SPI connection.
 *
 * Every SPI call is contained by invoke_pg_as_cpp so PostgreSQL longjmp does
 * not cross this non-trivial C++ owner. Explicit finish reports SPI status;
 * destruction performs the same cleanup best-effort and never throws.
 */
class scoped_spi_connection final {
  public:
    /** Opens one SPI connection or surfaces PostgreSQL's error as C++. */
    scoped_spi_connection() {
        invoke_pg_as_cpp([] {
            const int status = SPI_connect();
            if (status != SPI_OK_CONNECT) {
                ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg_internal("could not connect to SPI: status %d", status)));
            }
        });
        is_connected_ = true;
    }

    /** Closes an open connection without allowing cleanup failure to escape. */
    ~scoped_spi_connection() noexcept {
        if (!is_connected_) {
            return;
        }
        try {
            static_cast<void>(finish());
        } catch (...) {
            // Destruction cannot surface cleanup failure across a C boundary.
        }
    }

    scoped_spi_connection(const scoped_spi_connection&) = delete;
    scoped_spi_connection& operator=(const scoped_spi_connection&) = delete;
    scoped_spi_connection(scoped_spi_connection&&) = delete;
    scoped_spi_connection& operator=(scoped_spi_connection&&) = delete;

    /** Returns whether this owner currently holds an SPI connection. */
    [[nodiscard]] bool is_connected() const noexcept {
        return is_connected_;
    }

    /**
     * Executes one parameterized statement inside the PostgreSQL boundary.
     *
     * @return The unmodified status from SPI_execute_with_args.
     */
    [[nodiscard]] int execute_with_args(const char* command,
                                        int argument_count,
                                        Oid* argument_types,
                                        Datum* argument_values,
                                        const char* nulls,
                                        bool is_read_only,
                                        long tuple_count) {
        const bool connection_is_open = is_connected_;
        return invoke_pg_as_cpp([command,
                                 argument_count,
                                 argument_types,
                                 argument_values,
                                 nulls,
                                 is_read_only,
                                 tuple_count,
                                 connection_is_open] {
            if (!connection_is_open) {
                ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg_internal("cannot execute through a closed SPI scope")));
            }
            return SPI_execute_with_args(command,
                                         argument_count,
                                         argument_types,
                                         argument_values,
                                         nulls,
                                         is_read_only,
                                         tuple_count);
        });
    }

    /**
     * Closes this connection and returns PostgreSQL's SPI_finish status.
     * Repeated calls are harmless and return SPI_OK_FINISH.
     */
    [[nodiscard]] int finish() {
        if (!is_connected_) {
            return SPI_OK_FINISH;
        }
        const int status = invoke_pg_as_cpp([] { return SPI_finish(); });
        is_connected_ = false;
        return status;
    }

  private:
    bool is_connected_ = false;
};

static_assert(!std::is_copy_constructible_v<scoped_spi_connection>);
static_assert(!std::is_move_constructible_v<scoped_spi_connection>);
static_assert(std::is_nothrow_destructible_v<scoped_spi_connection>);

} // namespace bioseq::postgres::support
