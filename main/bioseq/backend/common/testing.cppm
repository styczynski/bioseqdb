module;
#include "common.h"
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>


export module bioseq.common.testing;
export import bioseq.common.mlogging;

class testRunListener : public Catch::EventListenerBase {
public:
    using Catch::EventListenerBase::EventListenerBase;

    void testRunStarting(Catch::TestRunInfo const&) override {
        log_configure(true);
        libassert::set_diff_highlighting(true);
    }
};

CATCH_REGISTER_LISTENER(testRunListener);
