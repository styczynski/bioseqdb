#pragma once

#include <filesystem>
#include <fstream>

#include "tests.hpp"

namespace tests {

extern "C" void test_atexit_bgw(::Datum arg);
extern "C" void test_atexit_bgw(::Datum arg) {
  cppgres::exception_guard([](auto arg) {
    auto bgw = cppgres::current_background_worker();
    int x = 123;
    cppgres::backend::atexit([=](int code) {
      std::ofstream f(cppgres::fmt::format("{}.result", MyProcPid), std::ios::binary);
      f << x;
    });
  })(arg);
}

add_test(backend_atexit, [](test_case &) {
  bool result = true;
  auto worker = cppgres::background_worker()
                    .name("test_atexit_bgw")
                    .type("test_atexit_bgw")
                    .library_name(get_library_name())
                    .function_name("test_atexit_bgw")
                    .flags(BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION)
                    .start_time(BgWorkerStart_RecoveryFinished);

  auto handle = worker.start();
  handle.wait_for_startup();
  pid_t pid = handle.get_pid();
  handle.wait_for_shutdown();

  auto filename = cppgres::fmt::format("{}.result", pid);
  std::ifstream istrm(filename, std::ios::binary);
  int x;
  istrm >> x;
  std::filesystem::remove(filename);
  result = result && _assert(x == 123);

  return result;
});

} // namespace tests
