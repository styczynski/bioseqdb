#pragma once

#include "tests.hpp"

namespace tests {

add_test(threading, ([](test_case &) {
           bool result = true;
           {
             cppgres::spi_executor spi;
             spi.execute("create table job (i int)");
           }

           cppgres::worker wrk;
           std::vector<std::future<int>> futures;
           std::thread t([&]() {
             for (int i = 0; i < 100; i++) {
               futures.emplace_back(wrk.post(
                   [](int i) {
                     cppgres::spi_executor spi;
                     auto res = spi.query<int32_t>("insert into job values ($1) returning i", i)
                                    .begin()[0];
                     return res;
                   },
                   i));
             }

             int sum = 0;
             for (auto &future : futures) {
               sum += future.get();
             }
             result = result && _assert(sum == 4950);

             wrk.post([&]() { wrk.terminate(); });
           });
           wrk.run();
           t.join();

           return result;
         }));

add_test(threading_non_main_thread, ([](test_case &) {
           bool result = true;

           cppgres::worker wrk;

           bool exception_raised = false;
           std::thread t([&]() {
             try {
               wrk.run();
             } catch (std::runtime_error &e) {
               exception_raised = true;
             }
           });

           t.join();
           result = result && _assert(exception_raised);

           return result;
         }));

} // namespace tests
