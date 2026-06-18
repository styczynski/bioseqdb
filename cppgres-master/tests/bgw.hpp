#pragma once

#include "tests.hpp"

namespace tests {

extern "C" void test_bgw(::Datum arg);
extern "C" inline void test_bgw(::Datum arg) {
  cppgres::exception_guard([](auto arg) {
    auto db_oid = cppgres::from_nullable_datum<cppgres::oid>(cppgres::nullable_datum(arg), OIDOID);
    auto bgw = cppgres::current_background_worker();
    bgw.connect(db_oid, std::nullopt, cppgres::background_worker_bypass_allow_connection());
    bgw.unblock_signals();
  })(arg);
}

add_test(bgworker, ([](test_case &) {
           bool result = true;

           auto worker = cppgres::background_worker()
                             .name("test_bgw")
                             .type("test_bgw")
                             .library_name(get_library_name())
                             .function_name("test_bgw")
                             .main_arg(cppgres::into_nullable_datum(cppgres::oid(MyDatabaseId)))
                             .flags(BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION)
                             .start_time(BgWorkerStart_RecoveryFinished);

           {
             // try a static start
             bool exception_raised = false;
             try {
               worker.start(false);
             } catch (std::runtime_error &) {
               exception_raised = true;
             }

             result = result && _assert(exception_raised);
           }

           auto handle = worker.start();
           handle.wait_for_startup();
           handle.terminate();
           handle.wait_for_shutdown();

           {
             bool exception_raised = false;
             try {
               handle.get_pid();
             } catch (cppgres::background_worker::worker_stopped &) {
               exception_raised = true;
             }

             result = result && _assert(exception_raised);
           }

           return result;
         }));

}; // namespace tests
