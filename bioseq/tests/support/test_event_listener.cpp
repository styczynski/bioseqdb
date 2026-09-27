/**
 * @file
 * @brief Catch2 listener that configures Bioseq test diagnostics.
 */
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>
#include <libassert/assert.hpp>

import bioseq.common.logging;

namespace bioseq::test_support {

/**
 * @brief Configure Bioseq diagnostics for the lifetime of a Catch2 test run.
 *
 * Catch2 owns the listener and invokes its externally named callbacks. The
 * listener has no per-case state and is registered exactly once below.
 */
class test_event_listener final : public Catch::EventListenerBase {
  public:
    /// @brief Inherits Catch2's listener constructors.
    using Catch::EventListenerBase::EventListenerBase;

    /**
     * @brief Enable test-mode logging and highlighted assertion differences.
     *
     * @param[in] test_run_info Catch2 metadata for the run being started.
     *
     * @note `testRunStarting` uses Catch2's externally mandated callback
     * spelling and runs once before the first test case is dispatched.
     */
    void testRunStarting([[maybe_unused]] const Catch::TestRunInfo& test_run_info) override {
        ::configure_logging(true);
        libassert::set_diff_highlighting(true);
    }
};

} // namespace bioseq::test_support

CATCH_REGISTER_LISTENER(bioseq::test_support::test_event_listener);
