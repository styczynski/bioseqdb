/*-------------------------------------------------------------------------
 *
 * common/pg_support/spi.cppm
 *
 * Minimal scoped SPI connection shared by PostgreSQL extension DSOs.
 *
 *-------------------------------------------------------------------------
 */
module;

extern "C" {
#include "postgres.h"
#include "executor/spi.h"
#include "utils/errcodes.h"
}

#include <type_traits>

export module bioseq.pg_support:spi;

import :error_boundary;

export namespace bioseq::pg_support {

/**
 * Own one nested SPI connection.
 *
 * Every SPI call is contained by invoke_pg_as_cpp, so a PostgreSQL longjmp is
 * converted before it can cross a scope containing this non-trivial owner.
 * Explicit finish() reports its SPI status; the destructor performs the same
 * cleanup best-effort and cannot throw.
 */
class ScopedSpiConnection final {
public:
    ScopedSpiConnection() {
        bioseq::pg_utils::invoke_pg_as_cpp([] {
            const int status = SPI_connect();
            if (status != SPI_OK_CONNECT) {
                ereport(
                    ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg_internal(
                         "could not connect to SPI: status %d",
                         status)));
            }
        });
        connected_ = true;
    }

    ~ScopedSpiConnection() noexcept {
        if (!connected_) {
            return;
        }
        try {
            static_cast<void>(finish());
        } catch (...) {
            // A destructor cannot surface cleanup failure across a C ABI.
        }
    }

    ScopedSpiConnection(const ScopedSpiConnection&) = delete;
    ScopedSpiConnection& operator=(const ScopedSpiConnection&) = delete;
    ScopedSpiConnection(ScopedSpiConnection&&) = delete;
    ScopedSpiConnection& operator=(ScopedSpiConnection&&) = delete;

    [[nodiscard]] bool connected() const noexcept {
        return connected_;
    }

    /**
     * Execute one parameterized statement inside the contained PG boundary.
     * The returned value is the unmodified SPI_execute_with_args status.
     */
    [[nodiscard]] int execute_with_args(
            const char* command,
            int argument_count,
            Oid* argument_types,
            Datum* argument_values,
            const char* nulls,
            bool read_only,
            long tuple_count) {
        const bool is_connected = connected_;
        return bioseq::pg_utils::invoke_pg_as_cpp(
            [command,
             argument_count,
             argument_types,
             argument_values,
             nulls,
             read_only,
             tuple_count,
             is_connected] {
                if (!is_connected) {
                    ereport(
                        ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg_internal(
                             "cannot execute through a closed SPI scope")));
                }
                return SPI_execute_with_args(
                    command,
                    argument_count,
                    argument_types,
                    argument_values,
                    nulls,
                    read_only,
                    tuple_count);
            });
    }

    /**
     * Close this connection and return the unmodified SPI_finish status.
     * Repeated calls are harmless and report SPI_OK_FINISH.
     */
    [[nodiscard]] int finish() {
        if (!connected_) {
            return SPI_OK_FINISH;
        }
        const int status = bioseq::pg_utils::invoke_pg_as_cpp([] {
            return SPI_finish();
        });
        connected_ = false;
        return status;
    }

private:
    bool connected_ = false;
};

static_assert(!std::is_copy_constructible_v<ScopedSpiConnection>);
static_assert(!std::is_move_constructible_v<ScopedSpiConnection>);
static_assert(std::is_nothrow_destructible_v<ScopedSpiConnection>);

}  // namespace bioseq::pg_support
