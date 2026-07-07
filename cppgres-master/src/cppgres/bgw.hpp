/**
 * \file
 */
#pragma once

#include "backend.hpp"
#include "datum.hpp"
#include "utils/maybe_ref.hpp"

namespace cppgres {
/**
 * @brief Background worker construction and operations
 */
struct background_worker {

  /**
   * @brief Initialize background worker specification
   *
   * @note Initializes pid notification to be directed to the current process by default,
   *       can be overriden by notify_pid(pid_t)
   */
  background_worker() { worker->bgw_notify_pid = ::MyProcPid; }

  /**
   * Initializes from a background worker specification reference
   * @param worker
   */
  background_worker(::BackgroundWorker &worker) : worker(worker) {}

  background_worker &name(std::string_view name) {
    size_t n = std::min(name.size(), static_cast<size_t>(sizeof(worker->bgw_name) - 1));
    std::copy_n(name.data(), n, worker->bgw_name);
    worker->bgw_name[n] = '\0';
    return *this;
  }
  std::string_view name() { return worker->bgw_name; }

  background_worker &type(std::string_view name) {
    size_t n = std::min(name.size(), static_cast<size_t>(sizeof(worker->bgw_type) - 1));
    std::copy_n(name.data(), n, worker->bgw_type);
    worker->bgw_type[n] = '\0';
    return *this;
  }
  std::string_view type() { return worker->bgw_type; }

  background_worker &library_name(std::string_view name) {
    size_t n = std::min(name.size(), static_cast<size_t>(sizeof(worker->bgw_library_name) - 1));
    std::copy_n(name.data(), n, worker->bgw_library_name);
    worker->bgw_library_name[n] = '\0';
    return *this;
  }
  std::string_view library_name() { return worker->bgw_library_name; }

  background_worker &function_name(std::string_view name) {
    size_t n = std::min(name.size(), static_cast<size_t>(sizeof(worker->bgw_function_name) - 1));
    std::copy_n(name.data(), n, worker->bgw_function_name);
    worker->bgw_function_name[n] = '\0';
    return *this;
  }
  std::string_view function_name() { return worker->bgw_function_name; }

  background_worker &start_time(BgWorkerStartTime time) {
    worker->bgw_start_time = time;
    return *this;
  }

  ::BgWorkerStartTime start_time() const { return worker->bgw_start_time; }

  background_worker &restart_time(int time) {
    worker->bgw_restart_time = time;
    return *this;
  }

  int restart_time() const { return worker->bgw_restart_time; }

  background_worker &flags(int flags) {
    worker->bgw_flags = flags;
    return *this;
  }

  int flags() const { return worker->bgw_flags; }

  background_worker &main_arg(datum datum) {
    worker->bgw_main_arg = datum;
    return *this;
  }

  datum main_arg() const { return datum(worker->bgw_main_arg); }

  background_worker &extra(std::string_view name) {
    size_t n = std::min(name.size(), static_cast<size_t>(sizeof(worker->bgw_extra) - 1));
    std::copy_n(name.data(), n, worker->bgw_extra);
    worker->bgw_extra[n] = '\0';
    return *this;
  }
  std::string_view extra() { return worker->bgw_extra; }

  background_worker &notify_pid(pid_t pid) {
    worker->bgw_notify_pid = pid;
    return *this;
  }

  pid_t notify_pid() const { return worker->bgw_notify_pid; }

  operator BackgroundWorker &() { return worker; }
  operator BackgroundWorker *() { return worker.operator->(); }

  struct worker_stopped : public std::exception {
    const char *what() const noexcept override { return "Background worker stopped"; }
  };

  struct worker_not_yet_started : public std::exception {
    const char *what() const noexcept override {
      return "Background worker hasn't been started yet";
    }
  };

  struct postmaster_died : public std::exception {
    const char *what() const noexcept override { return "postmaster died"; }
  };

  struct handle {
    handle() : handle_(nullptr) {}
    handle(::BackgroundWorkerHandle *handle) : handle_(handle) {}

    ::BackgroundWorkerHandle *operator->() { return handle_; }

    bool has_value() const { return handle_ != nullptr; }
    ::BackgroundWorkerHandle *value() { return handle_; }

    pid_t wait_for_startup() {
      if (has_value()) {
        pid_t pid;
        ffi_guard{::WaitForBackgroundWorkerStartup}(value(), &pid);
        return pid;
      }
      throw std::logic_error("Attempting to wait for a background worker with no handle");
    }

    void wait_for_shutdown() {
      if (has_value()) {
        ffi_guard{::WaitForBackgroundWorkerShutdown}(value());
        return;
      }
      throw std::logic_error("Attempting to wait for a background worker with no handle");
    }

    void terminate() {
      if (has_value()) {
        ffi_guard{::TerminateBackgroundWorker}(value());
        return;
      }
      throw std::logic_error("Attempting to terminate a background worker with no handle");
    }

    pid_t get_pid() {
      if (has_value()) {
        pid_t pid;
        auto rc = ffi_guard{::GetBackgroundWorkerPid}(value(), &pid);
        switch (rc) {
        case BGWH_STARTED:
          return pid;
        case BGWH_STOPPED:
          throw worker_stopped();
        case BGWH_NOT_YET_STARTED:
          throw worker_not_yet_started();
        case BGWH_POSTMASTER_DIED:
          throw postmaster_died();
        }
      }
      throw std::logic_error("Attempting to get a PID of a background worker with no handle");
    }

    const char *worker_type() { return ffi_guard{::GetBackgroundWorkerTypeByPid}(get_pid()); }

  private:
    ::BackgroundWorkerHandle *handle_;
  };

  handle start(bool dynamic = true) {
    if (!dynamic) {
      if (::IsUnderPostmaster || !::IsPostmasterEnvironment) {
        throw std::runtime_error(
            "static background worker can only be start in the postmaster process");
      }
      ffi_guard{::RegisterBackgroundWorker}(operator BackgroundWorker *());
      return {};
    }
    ::BackgroundWorkerHandle *handle;
    ffi_guard{::RegisterDynamicBackgroundWorker}(operator BackgroundWorker *(), &handle);
    return {handle};
  }

private:
  utils::maybe_ref<::BackgroundWorker> worker = {};
};

struct background_worker_database_conection_flag {
  virtual int flag() const { return 0; }
};

struct background_worker_bypass_allow_connection
    : public background_worker_database_conection_flag {
  int flag() const override { return BGWORKER_BYPASS_ALLOWCONN; }
};

#if PG_MAJORVERSION_NUM >= 17
struct background_worker_bypass_role_login_check
    : public background_worker_database_conection_flag {
  int flag() const override { return BGWORKER_BYPASS_ROLELOGINCHECK; }
};
#endif

struct current_background_worker : public background_worker {
  friend std::optional<current_background_worker> get_current_background_worker();

  /**
   * @brief gets current background worker's entry
   *
   * @throws std::logic_error if not in a background worker; to check the backend type
   *         use cppgres::backend::type()
   */
  current_background_worker() : background_worker(*::MyBgworkerEntry) {
    if (backend::type() != backend_type::bg_worker) {
      throw std::logic_error("can't access current background worker in a different backend type");
    }
  }

  void unblock_signals() { ffi_guard{::BackgroundWorkerUnblockSignals}(); }

  void block_signals() { ffi_guard{::BackgroundWorkerBlockSignals}(); }

  /**
   * @brief Connect to the database using db name and, optionally, username
   *
   * @tparam Flags connection flags of @ref cppgres::background_worker_database_conection_flag
   *               derived flags
   * @param dbname database name
   * @param user user name
   * @param flags connection flags of @ref cppgres::background_worker_database_conection_flag
   * derived flags
   */
  template <typename... Flags>
  void connect(std::string dbname, std::optional<std::string> user = std::nullopt, Flags... flags)
      requires(
          std::conjunction_v<std::is_base_of<background_worker_database_conection_flag, Flags>...>)
  {
    ffi_guard{::BackgroundWorkerInitializeConnection}(
        dbname.c_str(), user.has_value() ? user.value().c_str() : nullptr,
        (flags.flag() | ... | 0));
  }

  /**
   * @brief Connect to the database using db oid and, optionally, user oid
   *
   * @tparam Flags connection flags of @ref cppgres::background_worker_database_conection_flag
   *               derived flags
   * @param db database oid
   * @param user user oid
   * @param flags connection flags of @ref cppgres::background_worker_database_conection_flag
   * derived flags
   */
  template <typename... Flags>
  void connect(oid db, std::optional<oid> user = std::nullopt, Flags... flags) requires(
      std::conjunction_v<std::is_base_of<background_worker_database_conection_flag, Flags>...>)
  {
    ffi_guard{::BackgroundWorkerInitializeConnectionByOid}(
        db, user.has_value() ? user.value() : oid(InvalidOid), (flags.flag() | ... | 0));
  }
};

} // namespace cppgres
