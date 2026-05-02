/**
 * @file catch_listener.cppm
 * @brief Generic Catch2 listener every bench harness should register.
 *
 * `testing.cppm`'s `testRunStarting` hook calls `log_configure(true)`,
 * which sets fmtlog back to DBG. Anything we set in `main()` before
 * `Catch::Session::run()` gets overwritten the moment Catch2's run
 * begins. This listener re-clamps the level on every
 * `testCaseStarting` notification — those fire after `testRunStarting`,
 * so by the time any bench code runs the level is back to ERR.
 *
 * Without this, the bench's stderr fills with tens of thousands of
 * DBG/INF lines per run and the FTXUI dashboard / regression-check
 * table become unreadable. Every harness wants exactly the same
 * behaviour, so the listener is exported here once.
 *
 * Use:
 *
 *   #include <catch2/catch_session.hpp>
 *   import bench.scheduler.catch_listener;
 *   CATCH_REGISTER_LISTENER(bioseq::bench_scheduler::StandardLogClamp)
 */
module;

#include "common.h"

#include <catch2/reporters/catch_reporter_event_listener.hpp>

export module bench.scheduler.catch_listener;

import mlogging;

export {

namespace bioseq::bench_scheduler {

/**
 * @brief Clamp the global fmtlog level to a fixed value on every
 *        test case start.
 *
 * `kClampLevel` is a constexpr knob — a future harness that wants a
 * different floor (WRN, INF) can derive a thin alias:
 *
 *   class WarnClamp : public ClampLogLevelOnTestStart<fmtlog::WRN> {};
 *
 * Defaults to `ERR` because that's what every existing harness wants
 * — the FTXUI dashboard plus the regression-check table become
 * unreadable when DBG/INF/WRN noise is interleaved.
 */
template<fmtlog::LogLevel kClampLevel = fmtlog::ERR>
class ClampLogLevelOnTestStart : public Catch::EventListenerBase {
public:
    using Catch::EventListenerBase::EventListenerBase;
    void testCaseStarting(Catch::TestCaseInfo const&) override {
        fmtlog::setLogLevel(kClampLevel);
    }
};

/// Default ERR-clamping listener every harness in the project
/// registers. See `ClampLogLevelOnTestStart` for the templated form.
using StandardLogClamp = ClampLogLevelOnTestStart<fmtlog::ERR>;

}  // namespace bioseq::bench_scheduler

}  // export
