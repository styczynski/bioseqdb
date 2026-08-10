/**
 * @file
 * @brief Catch2 listener that configures Bioseq test diagnostics.
 */
module;

#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>
#include <libassert/assert.hpp>

export module bioseq.common.testing;

export import bioseq.common.logging;

namespace bioseq::test_support {

/// @brief Configures logging and assertion rendering when a Catch2 run starts.
class test_run_listener : public Catch::EventListenerBase {
public:
    /// @brief Inherits Catch2's listener constructors.
    using Catch::EventListenerBase::EventListenerBase;

    /**
     * @brief Enables test-mode logging and highlighted assertion differences.
     *
     * @param[in] test_run_info Catch2 metadata for the run being started.
     */
    void testRunStarting(
        [[maybe_unused]] const Catch::TestRunInfo& test_run_info) override {
        ::configure_logging(true);
        libassert::set_diff_highlighting(true);
    }
};

} // namespace bioseq::test_support

CATCH_REGISTER_LISTENER(bioseq::test_support::test_run_listener);
