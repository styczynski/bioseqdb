#pragma once

#include <future>
#include <queue>
#include <type_traits>

#include "imports.h"

#ifdef __linux__
#include <sys/syscall.h>
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include <unistd.h>
#if defined(__GLIBC__) && (__GLIBC__ < 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ < 30))
#include <sys/syscall.h>
static inline pid_t gettid() { return static_cast<pid_t>(syscall(SYS_gettid)); }
#endif
#elif __APPLE__
#include <pthread.h>
#endif

namespace cppgres {

#if defined(__linux__)
static inline bool is_main_thread() { return gettid() == getpid(); }
#elif defined(__APPLE__)
static inline bool is_main_thread() { return pthread_main_np() != 0; }
#else
#warning "is_main_thread() not implemented"
static inline bool is_main_thread() { return false; }
#endif

/**
 * @brief Single-threaded Postgres workload worker
 *
 * @warning Use extreme caution and care when handling workload – ensure the worker does not
 *          outlive the intended lifetime – and receives no interference – that is, no other
 *          threads should be doing any Postgres workloads while this worker is alive.
 */
struct worker {
  worker() : done(false), terminated(false) {}

  ~worker() { terminate(); }

  void terminate() {
    {
      std::scoped_lock lock(mutex);
      if (terminated)
        return;
      done = true;
      cv.notify_one();
    }
  }

  template <typename F, typename... Args>
  auto post(F &&f, Args &&...args) -> std::future<std::invoke_result_t<F, Args...>> {
    using ReturnType = std::invoke_result_t<F, Args...>;

    auto task = std::make_shared<std::packaged_task<ReturnType()>>(
        [f = std::move(f), ... args = std::move(args)]() { return f(args...); });
    std::future<ReturnType> result = task->get_future();

    {
      std::scoped_lock lock(mutex);
      tasks.emplace([task]() { (*task)(); });
    }
    cv.notify_one();
    return result;
  }

  /**
   * @brief Run the worker
   *
   * @throws std::runtime_error if called on a secondary thread
   */
  void run() {
    if (!is_main_thread()) {
      throw std::runtime_error("Worker can only run on main thread");
    }
    while (true) {
      std::function<void()> task;
      {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [&]() { return done.load() || !tasks.empty(); });
        if (done.load() && tasks.empty())
          break;
        task = std::move(tasks.front());
        tasks.pop();
      }
      task();
    }
    terminated = true;
  }

private:
  std::mutex mutex;
  std::condition_variable cv;
  std::queue<std::function<void()>> tasks;
  std::atomic<bool> done;
  std::atomic<bool> terminated;
};

} // namespace cppgres
