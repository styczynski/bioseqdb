/**
 * @file
 * @brief Sets up logging and assertion messages for Catch2 tests.
 */
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>
#include <libassert/assert.hpp>

import bioseq.common.logging;

namespace bioseq::test_support {

/** Enables Bioseq test logging and assertion differences when Catch2 starts a run. */
class test_event_listener final : public Catch::EventListenerBase {
  public:
    /// @brief Inherits Catch2's listener constructors.
    using Catch::EventListenerBase::EventListenerBase;

    /** Enables test logging and marks differences in failed assertions. */
    void testRunStarting([[maybe_unused]] const Catch::TestRunInfo& test_run_info) override {
        ::configure_logging(true);
        libassert::set_diff_highlighting(true);
    }
};

} // namespace bioseq::test_support

CATCH_REGISTER_LISTENER(bioseq::test_support::test_event_listener);
