#pragma once

#include "imports.h"
#include "memory.hpp"

namespace cppgres {

enum q { backend };

namespace backend_type {

/**
 * @brief Backend type
 *
 * A copy of Postgres' BackendType with minor renaming
 */
enum type {
  invalid = B_INVALID,
  backend = B_BACKEND,
  autovac_launcher = B_AUTOVAC_LAUNCHER,
  autovac_worker = B_AUTOVAC_WORKER,
  bg_worker = B_BG_WORKER,
  wal_sender = B_WAL_SENDER,
#if PG_MAJORVERSION_NUM >= 17
  slotsync_worker = B_SLOTSYNC_WORKER,
  standalone_backend = B_STANDALONE_BACKEND,
#endif
  archiver = B_ARCHIVER,
  bg_writer = B_BG_WRITER,
  checkpointer = B_CHECKPOINTER,
  startup = B_STARTUP,
  wal_receiver = B_WAL_RECEIVER,
#if PG_MAJORVERSION_NUM >= 17
  wal_summarizer = B_WAL_SUMMARIZER,
#endif
  wal_writer = B_WAL_WRITER,
  logger = B_LOGGER
};
}

/**
 * @brief Backend management
 */
struct backend {
  /**
   * @brief get current backend type
   * @return backend type
   */
  static backend_type::type type() { return static_cast<backend_type::type>(::MyBackendType); };

  /**
   * @brief Register a callback for when Postgres will be exiting
   *
   * This allows passing a lambda with a closure (not just a plain C function / lambda without a
   * closure), it'll initialize the closure in the top memory context.
   */
  template <typename T> requires requires(T t, int code) {
    { t(code) };
  }
  static void atexit(T &&func) {
    T *raw_mem = top_memory_context().alloc<T>();
    T *allocation = new (raw_mem) T(std::forward<T>(func));

    ffi_guard{::on_proc_exit}(
        [](int code, ::Datum datum) {
          T *func = reinterpret_cast<T *>(DatumGetPointer(datum));
          (*func)(code);
        },
        PointerGetDatum(allocation));
  }
};

} // namespace cppgres
