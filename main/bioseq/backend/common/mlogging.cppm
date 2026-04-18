/*
MIT License

Copyright (c) 2021 Meng Rao <raomeng1@gmail.com>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/
module;


//#define FMT_HEADER_ONLY
#include <type_traits>
#include <vector>
#include <chrono>
#include <atomic>
#include <thread>
#include <memory>
#include <mutex>
#include <limits>
#include <ios>

// For generic serialization
#include <rfl/json.hpp>
#include <rfl.hpp>

// FMT LOGIC
#define FMT_MODULE

#ifdef _MSVC_LANG
#  define FMT_CPLUSPLUS _MSVC_LANG
#else
#  define FMT_CPLUSPLUS __cplusplus
#endif

// Put all implementation-provided headers into the global module fragment
// to prevent attachment to this module.
#ifndef FMT_IMPORT_STD
#  include <algorithm>
#  include <bitset>
#  include <chrono>
#  include <cmath>
#  include <complex>
#  include <cstddef>
#  include <cstdint>
#  include <cstdio>
#  include <cstdlib>
#  include <cstring>
#  include <ctime>
#  include <exception>
#  if FMT_CPLUSPLUS > 202002L
#    include <expected>
#  endif
#  include <filesystem>
#  include <fstream>
#  include <functional>
#  include <iterator>
#  include <limits>
#  include <locale>
#  include <memory>
#  include <optional>
#  include <ostream>
#  include <source_location>
#  include <stdexcept>
#  include <string>
#  include <string_view>
#  include <system_error>
#  include <thread>
#  include <type_traits>
#  include <typeinfo>
#  include <utility>
#  include <variant>
#  include <vector>
#else
#  include <limits.h>
#  include <stdint.h>
#  include <stdio.h>
#  include <time.h>
#endif
#include <cerrno>
#include <climits>
#include <version>

#if __has_include(<cxxabi.h>)
#  include <cxxabi.h>
#endif
#if defined(_MSC_VER) || defined(__MINGW32__)
#  include <intrin.h>
#endif
#if defined __APPLE__ || defined(__FreeBSD__)
#  include <xlocale.h>
#endif
#if __has_include(<winapifamily.h>)
#  include <winapifamily.h>
#endif
#if (__has_include(<fcntl.h>) || defined(__APPLE__) || \
     defined(__linux__)) &&                            \
    (!defined(WINAPI_FAMILY) || (WINAPI_FAMILY == WINAPI_FAMILY_DESKTOP_APP))
#  include <fcntl.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#  ifndef _WIN32
#    include <unistd.h>
#  else
#    include <io.h>
#  endif
#endif
#ifdef _WIN32
#  if defined(__GLIBCXX__)
#    include <ext/stdio_filebuf.h>
#    include <ext/stdio_sync_filebuf.h>
#  endif
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

// FMT LOGIC END

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <processthreadsapi.h>
#else
#include <sys/syscall.h>
#include <unistd.h>
#endif

#ifdef _MSC_VER
#include <intrin.h>
#endif

#ifdef _WIN32
#define FAST_THREAD_LOCAL thread_local
#else
#define FAST_THREAD_LOCAL __thread
#endif

// define FMTLOG_BLOCK=1 if log statment should be blocked when queue is full, instead of discarding the msg
#ifndef FMTLOG_BLOCK
#define FMTLOG_BLOCK 0
#endif

#include "common.h"

export module mlogging;

// fmt logic

#ifdef FMT_IMPORT_STD
import std;
#endif

#define FMT_EXPORT export
#define FMT_BEGIN_EXPORT export {
#define FMT_END_EXPORT }

// If you define FMT_ATTACH_TO_GLOBAL_MODULE
//  - all declarations are detached from module 'fmt'
//  - the module behaves like a traditional static library, too
//  - all library symbols are mangled traditionally
//  - you can mix TUs with either importing or #including the {fmt} API
#ifdef FMT_ATTACH_TO_GLOBAL_MODULE
extern "C++" {
#endif

#ifndef FMT_OS
#  define FMT_OS 1
#endif

// All library-provided declarations and definitions must be in the module
// purview to be exported.
#include "fmt/args.h"
#include "fmt/chrono.h"
#include "fmt/color.h"
#include "fmt/compile.h"
#include "fmt/format.h"
#if FMT_OS
#  include "fmt/os.h"
#endif
#include "fmt/ostream.h"
#include "fmt/printf.h"
#include "fmt/ranges.h"
#include "fmt/std.h"
#include "fmt/xchar.h"

#include "src/format.cc"

#ifdef FMT_ATTACH_TO_GLOBAL_MODULE
}
#endif


// fmt logic end

namespace fmtlogdetail {
template<typename Arg>
struct UnrefPtr : std::false_type
{ using type = Arg; };

template<>
struct UnrefPtr<char*> : std::false_type
{ using type = char*; };

template<>
struct UnrefPtr<void*> : std::false_type
{ using type = void*; };

template<typename Arg>
struct UnrefPtr<std::shared_ptr<Arg>> : std::true_type
{ using type = Arg; };

template<typename Arg, typename D>
struct UnrefPtr<std::unique_ptr<Arg, D>> : std::true_type
{ using type = Arg; };

template<typename Arg>
struct UnrefPtr<Arg*> : std::true_type
{ using type = Arg; };

}; // namespace fmtlogdetail

template<int __ = 0>
class fmtlogT
{
public:
  enum LogLevel : uint8_t
  {
    DBG = 0,
    INF,
    WRN,
    ERR,
    OFF
  };

  // Preallocate thread queue for current thread
  static void preallocate() noexcept;

  // Set the file for logging
  static void setLogFile(const char* filename, bool truncate = false);

  // Set an existing FILE* for logging, if manageFp is false fmtlog will not buffer log internally
  // and will not close the FILE*
  static void setLogFile(FILE* fp, bool manageFp = false);

  // Collect log msgs from all threads and write to log file
  // If forceFlush = true, internal file buffer is flushed
  // User need to call poll() repeatedly if startPollingThread is not used
  static void poll(bool forceFlush = false);

  // Set flush delay in nanosecond
  // If there's msg older than ns in the buffer, flush will be triggered
  static void setFlushDelay(int64_t ns) noexcept;

  // If current msg has level >= flushLogLevel, flush will be triggered
  static void flushOn(LogLevel flushLogLevel) noexcept;

  // If file buffer has more than specified bytes, flush will be triggered
  static void setFlushBufSize(uint32_t bytes) noexcept;

  // callback signature user can register
  // ns: nanosecond timestamp
  // level: logLevel
  // location: full file path with line num, e.g: /home/raomeng/fmtlog/fmtlog.h:45
  // basePos: file base index in the location
  // threadName: thread id or the name user set with setThreadName
  // msg: full log msg with header
  // bodyPos: log body index in the msg
  // logFilePos: log file position of this msg
  typedef void (*LogCBFn)(int64_t ns, LogLevel level, fmt::string_view location, size_t basePos,
                          fmt::string_view threadName, fmt::string_view msg, size_t bodyPos,
                          size_t logFilePos);

  // Set a callback function for all log msgs with a mininum log level
  static void setLogCB(LogCBFn cb, LogLevel minCBLogLevel) noexcept;

  typedef void (*LogQFullCBFn)(void* userData);
  static void setLogQFullCB(LogQFullCBFn cb, void* userData) noexcept;

  // Close the log file and subsequent msgs will not be written into the file,
  // but callback function can still be used
  static void closeLogFile() noexcept;

  // Set log header pattern with fmt named arguments
  static void setHeaderPattern(const char* pattern);

  // Set a name for current thread, it'll be shown in {t} part in header pattern
  static void setThreadName(const char* name) noexcept;

  // Set current log level, lower level log msgs will be discarded
  static inline void setLogLevel(LogLevel logLevel) noexcept;

  // Get current log level
  static inline LogLevel getLogLevel() noexcept;

  // return true if passed log level is not lower than current log level
  static inline bool checkLogLevel(LogLevel logLevel) noexcept;

  // Run a polling thread in the background with a polling interval in ns
  // Note that user must not call poll() himself when the thread is running
  static void startPollingThread(int64_t pollInterval = 1000000000) noexcept;

  // Stop the polling thread
  static void stopPollingThread() noexcept;

  // https://github.com/MengRao/SPSC_Queue
  class SPSCVarQueueOPT
  {
  public:
    struct MsgHeader
    {
      inline void push(uint32_t sz) { *(volatile uint32_t*)&size = sz + sizeof(MsgHeader); }

      uint32_t size;
      uint32_t logId;
    };
    static constexpr uint32_t BLK_CNT = FMTLOG_QUEUE_SIZE / sizeof(MsgHeader);

    MsgHeader* allocMsg(uint32_t size) noexcept;

    MsgHeader* alloc(uint32_t size) {
      size += sizeof(MsgHeader);
      uint32_t blk_sz = (size + sizeof(MsgHeader) - 1) / sizeof(MsgHeader);
      if (blk_sz >= free_write_cnt) {
        uint32_t read_idx_cache = *(volatile uint32_t*)&read_idx;
        if (read_idx_cache <= write_idx) {
          free_write_cnt = BLK_CNT - write_idx;
          if (blk_sz >= free_write_cnt && read_idx_cache != 0) { // wrap around
            blk[0].size = 0;
            blk[write_idx].size = 1;
            write_idx = 0;
            free_write_cnt = read_idx_cache;
          }
        }
        else {
          free_write_cnt = read_idx_cache - write_idx;
        }
        if (free_write_cnt <= blk_sz) {
          return nullptr;
        }
      }
      MsgHeader* ret = &blk[write_idx];
      write_idx += blk_sz;
      free_write_cnt -= blk_sz;
      blk[write_idx].size = 0;
      return ret;
    }

    inline const MsgHeader* front() {
      uint32_t size = blk[read_idx].size;
      if (size == 1) { // wrap around
        read_idx = 0;
        size = blk[0].size;
      }
      if (size == 0) return nullptr;
      return &blk[read_idx];
    }

    inline void pop() {
      uint32_t blk_sz = (blk[read_idx].size + sizeof(MsgHeader) - 1) / sizeof(MsgHeader);
      *(volatile uint32_t*)&read_idx = read_idx + blk_sz;
    }

  private:
    alignas(64) MsgHeader blk[BLK_CNT] = {};
    uint32_t write_idx = 0;
    uint32_t free_write_cnt = BLK_CNT;

    alignas(128) uint32_t read_idx = 0;
  };

  struct ThreadBuffer
  {
    SPSCVarQueueOPT varq;
    bool shouldDeallocate = false;
    char name[32];
    size_t nameSize;
  };

  // https://github.com/MengRao/tscns
  class TSCNS
  {
  public:
    static const int64_t NsPerSec = 1000000000;

    void init(int64_t init_calibrate_ns = 20000000, int64_t calibrate_interval_ns = 3 * NsPerSec) {
      calibate_interval_ns_ = calibrate_interval_ns;
      int64_t base_tsc, base_ns;
      syncTime(base_tsc, base_ns);
      int64_t expire_ns = base_ns + init_calibrate_ns;
      while (rdsysns() < expire_ns) std::this_thread::yield();
      int64_t delayed_tsc, delayed_ns;
      syncTime(delayed_tsc, delayed_ns);
      double init_ns_per_tsc = (double)(delayed_ns - base_ns) / (delayed_tsc - base_tsc);
      saveParam(base_tsc, base_ns, 0, init_ns_per_tsc);
    }

    void calibrate() {
      if (rdtsc() < next_calibrate_tsc_) return;
      int64_t tsc, ns;
      syncTime(tsc, ns);
      int64_t ns_err = tsc2ns(tsc) - ns;
      if (ns_err > 1000000) ns_err = 1000000;
      if (ns_err < -1000000) ns_err = -1000000;
      double new_ns_per_tsc =
        ns_per_tsc_ * (1.0 - (ns_err + ns_err - base_ns_err_) / ((tsc - base_tsc_) * ns_per_tsc_));
      saveParam(tsc, ns, ns_err, new_ns_per_tsc);
    }

    static inline int64_t rdtsc() {
#ifdef _MSC_VER
      return __rdtsc();
#elif defined(__i386__) || defined(__x86_64__) || defined(__amd64__)
      return __builtin_ia32_rdtsc();
#else
      return rdsysns();
#endif
    }

    inline int64_t tsc2ns(int64_t tsc) const {
      while (true) {
        uint32_t before_seq = param_seq_.load(std::memory_order_acquire) & ~1;
        std::atomic_signal_fence(std::memory_order_acq_rel);
        int64_t ns = base_ns_ + (int64_t)((tsc - base_tsc_) * ns_per_tsc_);
        std::atomic_signal_fence(std::memory_order_acq_rel);
        uint32_t after_seq = param_seq_.load(std::memory_order_acquire);
        if (before_seq == after_seq) return ns;
      }
    }

    inline int64_t rdns() const { return tsc2ns(rdtsc()); }

    static inline int64_t rdsysns() {
      using namespace std::chrono;
      return duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();
    }

    double getTscGhz() const { return 1.0 / ns_per_tsc_; }

    // Linux kernel sync time by finding the first trial with tsc diff < 50000
    // We try several times and return the one with the mininum tsc diff.
    // Note that MSVC has a 100ns resolution clock, so we need to combine those ns with the same
    // value, and drop the first and the last value as they may not scan a full 100ns range
    static void syncTime(int64_t& tsc_out, int64_t& ns_out) {
#ifdef _MSC_VER
      const int N = 15;
#else
      const int N = 3;
#endif
      int64_t tsc[N + 1];
      int64_t ns[N + 1];

      tsc[0] = rdtsc();
      for (int i = 1; i <= N; i++) {
        ns[i] = rdsysns();
        tsc[i] = rdtsc();
      }

#ifdef _MSC_VER
      int j = 1;
      for (int i = 2; i <= N; i++) {
        if (ns[i] == ns[i - 1]) continue;
        tsc[j - 1] = tsc[i - 1];
        ns[j++] = ns[i];
      }
      j--;
#else
      int j = N + 1;
#endif

      int best = 1;
      for (int i = 2; i < j; i++) {
        if (tsc[i] - tsc[i - 1] < tsc[best] - tsc[best - 1]) best = i;
      }
      tsc_out = (tsc[best] + tsc[best - 1]) >> 1;
      ns_out = ns[best];
    }

    void saveParam(int64_t base_tsc, int64_t sys_ns, int64_t base_ns_err, double new_ns_per_tsc) {
      base_ns_err_ = base_ns_err;
      next_calibrate_tsc_ = base_tsc + (int64_t)((calibate_interval_ns_ - 1000) / new_ns_per_tsc);
      uint32_t seq = param_seq_.load(std::memory_order_relaxed);
      param_seq_.store(++seq, std::memory_order_release);
      std::atomic_signal_fence(std::memory_order_acq_rel);
      base_tsc_ = base_tsc;
      base_ns_ = sys_ns + base_ns_err;
      ns_per_tsc_ = new_ns_per_tsc;
      std::atomic_signal_fence(std::memory_order_acq_rel);
      param_seq_.store(++seq, std::memory_order_release);
    }

    alignas(64) std::atomic<uint32_t> param_seq_ = 0;
    double ns_per_tsc_;
    int64_t base_tsc_;
    int64_t base_ns_;
    int64_t calibate_interval_ns_;
    int64_t base_ns_err_;
    int64_t next_calibrate_tsc_;
  };

  void init() {
    tscns.init();
    currentLogLevel = INF;
  }

  using Context = fmt::format_context;
  using MemoryBuffer = fmt::basic_memory_buffer<char, 10000>;
  typedef const char* (*FormatToFn)(fmt::string_view format, const char* data, MemoryBuffer& out,
                                    int& argIdx, std::vector<fmt::basic_format_arg<Context>>& args);

  static void registerLogInfo(uint32_t& logId, FormatToFn fn, const char* location, LogLevel level,
                              fmt::string_view fmtString) noexcept;

  static void vformat_to(MemoryBuffer& out, fmt::string_view fmt, fmt::format_args args);

  static size_t formatted_size(fmt::string_view fmt, fmt::format_args args);

  static void vformat_to(char* out, fmt::string_view fmt, fmt::format_args args);

  static typename SPSCVarQueueOPT::MsgHeader* allocMsg(uint32_t size, bool logQFullCB) noexcept;

  TSCNS tscns;

  volatile LogLevel currentLogLevel;
  static FAST_THREAD_LOCAL ThreadBuffer* threadBuffer;

  template<typename Arg>
  static inline constexpr bool isNamedArg() {
    return fmt::detail::is_named_arg<fmt::remove_cvref_t<Arg>>::value;
  }

  template<typename Arg>
  struct unNamedType
  { using type = Arg; };

  template<typename Arg>
  struct unNamedType<fmt::detail::named_arg<char, Arg>>
  { using type = Arg; };

#if FMT_USE_NONTYPE_TEMPLATE_ARGS
  template<typename Arg, size_t N, fmt::detail::fixed_string<char, N> Str>
  struct unNamedType<fmt::detail::static_named_arg<Arg, char, N, Str>>
  { using type = Arg; };
#endif

  template<typename Arg>
  static inline constexpr bool isCstring() {
    return fmt::detail::mapped_type_constant<Arg, char>::value == fmt::detail::type::cstring_type;
  }

  template<typename Arg>
  static inline constexpr bool isString() {
    return fmt::detail::mapped_type_constant<Arg, char>::value == fmt::detail::type::string_type;
  }

  template<typename Arg>
  static inline constexpr bool needCallDtor() {
    using ArgType = fmt::remove_cvref_t<Arg>;
    if constexpr (isNamedArg<Arg>()) {
      return needCallDtor<typename unNamedType<ArgType>::type>();
    }
    if constexpr (isString<Arg>()) return false;
    return !std::is_trivially_destructible<ArgType>::value;
  }

  template<size_t CstringIdx>
  static inline constexpr size_t getArgSizes(size_t* cstringSize) {
    return 0;
  }

  template<size_t CstringIdx, typename Arg, typename... Args>
  static inline constexpr size_t getArgSizes(size_t* cstringSize, const Arg& arg,
                                             const Args&... args) {
    if constexpr (isNamedArg<Arg>()) {
      return getArgSizes<CstringIdx>(cstringSize, arg.value, args...);
    }
    else if constexpr (isCstring<Arg>()) {
      size_t len = strlen(arg) + 1;
      cstringSize[CstringIdx] = len;
      return len + getArgSizes<CstringIdx + 1>(cstringSize, args...);
    }
    else if constexpr (isString<Arg>()) {
      size_t len = arg.size() + 1;
      return len + getArgSizes<CstringIdx>(cstringSize, args...);
    }
    else {
      return sizeof(Arg) + getArgSizes<CstringIdx>(cstringSize, args...);
    }
  }

  template<size_t CstringIdx>
  static inline constexpr char* encodeArgs(size_t* cstringSize, char* out) {
    return out;
  }

  template<size_t CstringIdx, typename Arg, typename... Args>
  static inline constexpr char* encodeArgs(size_t* cstringSize, char* out, Arg&& arg,
                                           Args&&... args) {
    if constexpr (isNamedArg<Arg>()) {
      return encodeArgs<CstringIdx>(cstringSize, out, arg.value, std::forward<Args>(args)...);
    }
    else if constexpr (isCstring<Arg>()) {
      memcpy(out, arg, cstringSize[CstringIdx]);
      return encodeArgs<CstringIdx + 1>(cstringSize, out + cstringSize[CstringIdx],
                                        std::forward<Args>(args)...);
    }
    else if constexpr (isString<Arg>()) {
      size_t len = arg.size();
      memcpy(out, arg.data(), len);
      out[len] = 0;
      return encodeArgs<CstringIdx>(cstringSize, out + len + 1, std::forward<Args>(args)...);
    }
    else {
      // If Arg has alignment >= 16, gcc could emit aligned move instructions(e.g. movdqa) for
      // placement new even if the *out* is misaligned, which would cause segfault. So we use memcpy
      // when possible
      if constexpr (std::is_trivially_copyable_v<fmt::remove_cvref_t<Arg>>) {
        memcpy(out, &arg, sizeof(Arg));
      }
      else {
        new (out) fmt::remove_cvref_t<Arg>(std::forward<Arg>(arg));
      }
      return encodeArgs<CstringIdx>(cstringSize, out + sizeof(Arg), std::forward<Args>(args)...);
    }
  }

  template<size_t Idx, size_t NamedIdx>
  static inline constexpr void storeNamedArgs(fmt::detail::named_arg_info<char>* named_args_store) {
  }

  template<size_t Idx, size_t NamedIdx, typename Arg, typename... Args>
  static inline constexpr void storeNamedArgs(fmt::detail::named_arg_info<char>* named_args_store,
                                              const Arg& arg, const Args&... args) {
    if constexpr (isNamedArg<Arg>()) {
      named_args_store[NamedIdx] = {arg.name, Idx};
      storeNamedArgs<Idx + 1, NamedIdx + 1>(named_args_store, args...);
    }
    else {
      storeNamedArgs<Idx + 1, NamedIdx>(named_args_store, args...);
    }
  }

  template<bool ValueOnly, size_t Idx, size_t DestructIdx>
  static inline const char* decodeArgs(const char* in, fmt::basic_format_arg<Context>* args,
                                       const char** destruct_args) {
    return in;
  }

  template<bool ValueOnly, size_t Idx, size_t DestructIdx, typename Arg, typename... Args>
  static inline const char* decodeArgs(const char* in, fmt::basic_format_arg<Context>* args,
                                       const char** destruct_args) {
    using namespace fmtlogdetail;
    using ArgType = fmt::remove_cvref_t<Arg>;
    if constexpr (isNamedArg<ArgType>()) {
      return decodeArgs<ValueOnly, Idx, DestructIdx, typename unNamedType<ArgType>::type, Args...>(
        in, args, destruct_args);
    }
    else if constexpr (isCstring<Arg>() || isString<Arg>()) {
      size_t size = strlen(in);
      fmt::string_view v(in, size);
      if constexpr (ValueOnly) {
        fmt::detail::value<Context>& value_ = *(fmt::detail::value<Context>*)(args + Idx);
        value_ = v;
      }
      else {
        args[Idx] = v;
      }
      return decodeArgs<ValueOnly, Idx + 1, DestructIdx, Args...>(in + size + 1, args,
                                                                  destruct_args);
    }
    else {
      if constexpr (ValueOnly) {
        fmt::detail::value<Context>& value_ = *(fmt::detail::value<Context>*)(args + Idx);
        if constexpr (UnrefPtr<ArgType>::value) {
          value_ = **(ArgType*)in;
        }
        else {
          value_ = *(ArgType*)in;
        }
      }
      else {
        if constexpr (UnrefPtr<ArgType>::value) {
          args[Idx] = **(ArgType*)in;
        }
        else {
          args[Idx] = *(ArgType*)in;
        }
      }

      if constexpr (needCallDtor<Arg>()) {
        destruct_args[DestructIdx] = in;
        return decodeArgs<ValueOnly, Idx + 1, DestructIdx + 1, Args...>(in + sizeof(ArgType), args,
                                                                        destruct_args);
      }
      else {
        return decodeArgs<ValueOnly, Idx + 1, DestructIdx, Args...>(in + sizeof(ArgType), args,
                                                                    destruct_args);
      }
    }
  }

  template<size_t DestructIdx>
  static inline void destructArgs(const char** destruct_args) {}

  template<size_t DestructIdx, typename Arg, typename... Args>
  static inline void destructArgs(const char** destruct_args) {
    using ArgType = fmt::remove_cvref_t<Arg>;
    if constexpr (isNamedArg<ArgType>()) {
      destructArgs<DestructIdx, typename unNamedType<ArgType>::type, Args...>(destruct_args);
    }
    else if constexpr (needCallDtor<Arg>()) {
      ((ArgType*)destruct_args[DestructIdx])->~ArgType();
      destructArgs<DestructIdx + 1, Args...>(destruct_args);
    }
    else {
      destructArgs<DestructIdx, Args...>(destruct_args);
    }
  }

  template<typename... Args>
  static const char* formatTo(fmt::string_view format, const char* data, MemoryBuffer& out,
                              int& argIdx, std::vector<fmt::basic_format_arg<Context>>& args) {
    constexpr size_t num_args = sizeof...(Args);
    constexpr size_t num_dtors = fmt::detail::count<needCallDtor<Args>()...>();
    const char* dtor_args[std::max(num_dtors, (size_t)1)];
    const char* ret;
    if (argIdx < 0) {
      argIdx = (int)args.size();
      args.resize(argIdx + num_args);
      ret = decodeArgs<false, 0, 0, Args...>(data, args.data() + argIdx, dtor_args);
    }
    else {
      ret = decodeArgs<true, 0, 0, Args...>(data, args.data() + argIdx, dtor_args);
    }
    vformat_to(out, format, fmt::basic_format_args(args.data() + argIdx, num_args));
    destructArgs<0, Args...>(dtor_args);

    return ret;
  }

  template<bool Reorder, typename... Args>
  static fmt::string_view unNameFormat(fmt::string_view in, uint32_t* reorderIdx,
                                       const Args&... args) {
    constexpr size_t num_named_args = fmt::detail::count<isNamedArg<Args>()...>();
    if constexpr (num_named_args == 0) {
      return in;
    }
    const char* begin = in.data();
    const char* p = begin;
    std::unique_ptr<char[]> unnamed_str(new char[in.size() + 1 + num_named_args * 5]);
    fmt::detail::named_arg_info<char> named_args[std::max(num_named_args, (size_t)1)];
    storeNamedArgs<0, 0>(named_args, args...);

    char* out = (char*)unnamed_str.get();
    uint8_t arg_idx = 0;
    while (true) {
      auto c = *p++;
      if (!c) {
        size_t copy_size = p - begin - 1;
        memcpy(out, begin, copy_size);
        out += copy_size;
        break;
      }
      if (c != '{') continue;
      size_t copy_size = p - begin;
      memcpy(out, begin, copy_size);
      out += copy_size;
      begin = p;
      c = *p++;
      if (!c) fmt::report_error("invalid format string");
      if (fmt::detail::is_name_start(c)) {
        while ((fmt::detail::is_name_start(c = *p) || ('0' <= c && c <= '9'))) {
          ++p;
        }
        fmt::string_view name(begin, p - begin);
        int id = -1;
        for (size_t i = 0; i < num_named_args; ++i) {
          if (named_args[i].name == name) {
            id = named_args[i].id;
            break;
          }
        }
        if (id < 0) fmt::report_error("invalid format string");
        if constexpr (Reorder) {
          reorderIdx[id] = arg_idx++;
        }
        else {
          out = fmt::format_to(out, "{}", id);
        }
      }
      else {
        *out++ = c;
      }
      begin = p;
    }
    const char* ptr = unnamed_str.release();
    return fmt::string_view(ptr, out - ptr);
  }

public:
  template<typename... Args>
  inline void log(
    uint32_t& logId, int64_t tsc, const char* location, LogLevel level,
    fmt::format_string<typename fmtlogdetail::UnrefPtr<fmt::remove_cvref_t<Args>>::type...> format,
    Args&&... args) noexcept {
    if (!logId) {
      auto unnamed_format = unNameFormat<false>(fmt::string_view(format), nullptr, args...);
      registerLogInfo(logId, formatTo<Args...>, location, level, unnamed_format);
    }
    constexpr size_t num_cstring = fmt::detail::count<isCstring<Args>()...>();
    size_t cstringSizes[std::max(num_cstring, (size_t)1)];
    uint32_t alloc_size = 8 + (uint32_t)getArgSizes<0>(cstringSizes, args...);
    bool q_full_cb = true;
    do {
      if (auto header = allocMsg(alloc_size, q_full_cb)) {
        header->logId = logId;
        char* out = (char*)(header + 1);
        *(int64_t*)out = tsc;
        out += 8;
        encodeArgs<0>(cstringSizes, out, std::forward<Args>(args)...);
        header->push(alloc_size);
        break;
      }
      q_full_cb = false;
    } while (FMTLOG_BLOCK);
  }

  template<typename... Args>
  inline void logOnce(const char* location, LogLevel level, fmt::format_string<Args...> format,
                      Args&&... args) {
    fmt::string_view sv(format);
    auto&& fmt_args = fmt::make_format_args(args...);
    uint32_t fmt_size = formatted_size(sv, fmt_args);
    uint32_t alloc_size = 8 + 8 + fmt_size;
    bool q_full_cb = true;
    do {
      if (auto header = allocMsg(alloc_size, q_full_cb)) {
        header->logId = (uint32_t)level;
        char* out = (char*)(header + 1);
        *(int64_t*)out = tscns.rdtsc();
        out += 8;
        *(const char**)out = location;
        out += 8;
        vformat_to(out, sv, fmt_args);
        header->push(alloc_size);
        break;
      }
      q_full_cb = false;
    } while (FMTLOG_BLOCK);
  }
};

export using fmtlog = fmtlogT<>;

template<int _>
FAST_THREAD_LOCAL typename fmtlogT<_>::ThreadBuffer* fmtlogT<_>::threadBuffer;

export template<int __ = 0>
struct fmtlogWrapper
{ static fmtlog impl; };

template<int _>
fmtlog fmtlogWrapper<_>::impl;

template<int _>
inline void fmtlogT<_>::setLogLevel(LogLevel logLevel) noexcept {
  fmtlogWrapper<>::impl.currentLogLevel = logLevel;
}

template<int _>
inline typename fmtlogT<_>::LogLevel fmtlogT<_>::getLogLevel() noexcept {
  return fmtlogWrapper<>::impl.currentLogLevel;
}

template<int _>
inline bool fmtlogT<_>::checkLogLevel(LogLevel logLevel) noexcept {
#ifdef FMTLOG_NO_CHECK_LEVEL
  return true;
#else
  return logLevel >= fmtlogWrapper<>::impl.currentLogLevel;
#endif
}


namespace {
void fmtlogEmptyFun(void*) {
}
} // namespace

template<int ___ = 0>
class fmtlogDetailT
{
public:
  // https://github.com/MengRao/str
  template<size_t SIZE>
  class Str
  {
  public:
    static const int Size = SIZE;
    char s[SIZE];

    Str() {}
    Str(const char* p) { *this = *(const Str<SIZE>*)p; }

    char& operator[](int i) { return s[i]; }
    char operator[](int i) const { return s[i]; }

    template<typename T>
    void fromi(T num) {
      if constexpr (Size & 1) {
        s[Size - 1] = '0' + (num % 10);
        num /= 10;
      }
      switch (Size & -2) {
        case 18: *(uint16_t*)(s + 16) = *(uint16_t*)(digit_pairs + ((num % 100) << 1)); num /= 100;
        case 16: *(uint16_t*)(s + 14) = *(uint16_t*)(digit_pairs + ((num % 100) << 1)); num /= 100;
        case 14: *(uint16_t*)(s + 12) = *(uint16_t*)(digit_pairs + ((num % 100) << 1)); num /= 100;
        case 12: *(uint16_t*)(s + 10) = *(uint16_t*)(digit_pairs + ((num % 100) << 1)); num /= 100;
        case 10: *(uint16_t*)(s + 8) = *(uint16_t*)(digit_pairs + ((num % 100) << 1)); num /= 100;
        case 8: *(uint16_t*)(s + 6) = *(uint16_t*)(digit_pairs + ((num % 100) << 1)); num /= 100;
        case 6: *(uint16_t*)(s + 4) = *(uint16_t*)(digit_pairs + ((num % 100) << 1)); num /= 100;
        case 4: *(uint16_t*)(s + 2) = *(uint16_t*)(digit_pairs + ((num % 100) << 1)); num /= 100;
        case 2: *(uint16_t*)(s + 0) = *(uint16_t*)(digit_pairs + ((num % 100) << 1)); num /= 100;
      }
    }

    static constexpr const char* digit_pairs = "00010203040506070809"
                                               "10111213141516171819"
                                               "20212223242526272829"
                                               "30313233343536373839"
                                               "40414243444546474849"
                                               "50515253545556575859"
                                               "60616263646566676869"
                                               "70717273747576777879"
                                               "80818283848586878889"
                                               "90919293949596979899";
  };

  fmtlogDetailT()
    : flushDelay(3000000000) {
    args.reserve(4096);
    args.resize(parttenArgSize);

    fmtlogWrapper<>::impl.init();
    resetDate();
    fmtlog::setLogFile(stdout);
    setHeaderPattern("{HMSf} {s:<16} {l}[{t:<6}] ");
    logInfos.reserve(32);
    bgLogInfos.reserve(128);
    bgLogInfos.emplace_back(nullptr, nullptr, fmtlog::DBG, fmt::string_view());
    bgLogInfos.emplace_back(nullptr, nullptr, fmtlog::INF, fmt::string_view());
    bgLogInfos.emplace_back(nullptr, nullptr, fmtlog::WRN, fmt::string_view());
    bgLogInfos.emplace_back(nullptr, nullptr, fmtlog::ERR, fmt::string_view());
    threadBuffers.reserve(8);
    bgThreadBuffers.reserve(8);
    memset(membuf.data(), 0, membuf.capacity());
  }

  ~fmtlogDetailT() {
    stopPollingThread();
    poll(true);
    closeLogFile();
  }

  void setHeaderPattern(const char* pattern) {
    if (shouldDeallocateHeader) delete[] headerPattern.data();
    using namespace fmt::literals;
    for (int i = 0; i < parttenArgSize; i++) {
      reorderIdx[i] = parttenArgSize - 1;
    }
    headerPattern = fmtlog::unNameFormat<true>(
      pattern, reorderIdx, "a"_a = "", "b"_a = "", "C"_a = "", "Y"_a = "", "m"_a = "", "d"_a = "",
      "t"_a = "thread name", "F"_a = "", "f"_a = "", "e"_a = "", "S"_a = "", "M"_a = "", "H"_a = "",
      "l"_a = fmtlog::LogLevel(), "s"_a = "fmtlog.cc:123", "g"_a = "/home/raomeng/fmtlog/fmtlog.cc:123", "Ymd"_a = "",
      "HMS"_a = "", "HMSe"_a = "", "HMSf"_a = "", "HMSF"_a = "", "YmdHMS"_a = "", "YmdHMSe"_a = "", "YmdHMSf"_a = "",
      "YmdHMSF"_a = "");
    shouldDeallocateHeader = headerPattern.data() != pattern;

    setArg<0>(fmt::string_view(weekdayName.s, 3));
    setArg<1>(fmt::string_view(monthName.s, 3));
    setArg<2>(fmt::string_view(&year[2], 2));
    setArg<3>(fmt::string_view(year.s, 4));
    setArg<4>(fmt::string_view(month.s, 2));
    setArg<5>(fmt::string_view(day.s, 2));
    setArg<6>(fmt::string_view());
    setArg<7>(fmt::string_view(nanosecond.s, 9));
    setArg<8>(fmt::string_view(nanosecond.s, 6));
    setArg<9>(fmt::string_view(nanosecond.s, 3));
    setArg<10>(fmt::string_view(second.s, 2));
    setArg<11>(fmt::string_view(minute.s, 2));
    setArg<12>(fmt::string_view(hour.s, 2));
    setArg<13>(fmt::string_view(logLevel.s, 3));
    setArg<14>(fmt::string_view());
    setArg<15>(fmt::string_view());
    setArg<16>(fmt::string_view(year.s, 10)); // Ymd
    setArg<17>(fmt::string_view(hour.s, 8));  // HMS
    setArg<18>(fmt::string_view(hour.s, 12)); // HMSe
    setArg<19>(fmt::string_view(hour.s, 15)); // HMSf
    setArg<20>(fmt::string_view(hour.s, 18)); // HMSF
    setArg<21>(fmt::string_view(year.s, 19));   // YmdHMS
    setArg<22>(fmt::string_view(year.s, 23));   // YmdHMSe
    setArg<23>(fmt::string_view(year.s, 26));   // YmdHMSf
    setArg<24>(fmt::string_view(year.s, 29));   // YmdHMSF
  }

  class ThreadBufferDestroyer
  {
  public:
    explicit ThreadBufferDestroyer() {}

    void threadBufferCreated() {}

    ~ThreadBufferDestroyer() {
      if (fmtlog::threadBuffer != nullptr) {
        fmtlog::threadBuffer->shouldDeallocate = true;
        fmtlog::threadBuffer = nullptr;
      }
    }
  };

  struct StaticLogInfo
  {
    // Constructor
    constexpr StaticLogInfo(fmtlog::FormatToFn fn, const char* loc, fmtlog::LogLevel level, fmt::string_view fmtString)
      : formatToFn(fn)
      , formatString(fmtString)
      , location(loc)
      , logLevel(level)
      , argIdx(-1) {}

    void processLocation() {
      size_t size = strlen(location);
      const char* p = location + size;
      if (size > 255) {
        location = p - 255;
      }
      endPos = p - location;
      const char* base = location;
      while (p > location) {
        char c = *--p;
        if (c == '/' || c == '\\') {
          base = p + 1;
          break;
        }
      }
      basePos = base - location;
    }

    inline fmt::string_view getBase() { return fmt::string_view(location + basePos, endPos - basePos); }

    inline fmt::string_view getLocation() { return fmt::string_view(location, endPos); }

    fmtlog::FormatToFn formatToFn;
    fmt::string_view formatString;
    const char* location;
    uint8_t basePos;
    uint8_t endPos;
    fmtlog::LogLevel logLevel;
    int argIdx;
  };

  static thread_local ThreadBufferDestroyer sbc;
  int64_t midnightNs;
  fmt::string_view headerPattern;
  bool shouldDeallocateHeader = false;
  FILE* outputFp = nullptr;
  bool manageFp = false;
  size_t fpos = 0; // file position of membuf, used only when manageFp == true
  int64_t flushDelay;
  int64_t nextFlushTime = (std::numeric_limits<int64_t>::max)();
  uint32_t flushBufSize = 8 * 1024;
  fmtlog::LogLevel flushLogLevel = fmtlog::OFF;
  std::mutex bufferMutex;
  std::vector<fmtlog::ThreadBuffer*> threadBuffers;
  struct HeapNode
  {
    HeapNode(fmtlog::ThreadBuffer* buffer)
      : tb(buffer) {}

    fmtlog::ThreadBuffer* tb;
    const fmtlog::SPSCVarQueueOPT::MsgHeader* header = nullptr;
  };
  std::vector<HeapNode> bgThreadBuffers;
  std::mutex logInfoMutex;
  std::vector<StaticLogInfo> logInfos;
  std::vector<StaticLogInfo> bgLogInfos;

  fmtlog::LogCBFn logCB = nullptr;
  fmtlog::LogLevel minCBLogLevel;
  fmtlog::LogQFullCBFn logQFullCB = fmtlogEmptyFun;
  void* logQFullCBArg = nullptr;

  fmtlog::MemoryBuffer membuf;

  const static int parttenArgSize = 25;
  uint32_t reorderIdx[parttenArgSize];
  Str<3> weekdayName;
  Str<3> monthName;
  Str<4> year;
  char dash1 = '-';
  Str<2> month;
  char dash2 = '-';
  Str<2> day;
  char space = ' ';
  Str<2> hour;
  char colon1 = ':';
  Str<2> minute;
  char colon2 = ':';
  Str<2> second;
  char dot1 = '.';
  Str<9> nanosecond;
  Str<3> logLevel;
  std::vector<fmt::basic_format_arg<fmtlog::Context>> args;

  volatile bool threadRunning = false;
  std::thread thr;

  void resetDate() {
    time_t rawtime = fmtlogWrapper<>::impl.tscns.rdns() / 1000000000;
    struct tm* timeinfo = localtime(&rawtime);
    timeinfo->tm_sec = timeinfo->tm_min = timeinfo->tm_hour = 0;
    midnightNs = mktime(timeinfo) * 1000000000;
    year.fromi(1900 + timeinfo->tm_year);
    month.fromi(1 + timeinfo->tm_mon);
    day.fromi(timeinfo->tm_mday);
    const char* weekdays[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    weekdayName = weekdays[timeinfo->tm_wday];
    const char* monthNames[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    monthName = monthNames[timeinfo->tm_mon];
  }

  void preallocate() {
    if (fmtlog::threadBuffer) return;
    fmtlog::threadBuffer = new fmtlog::ThreadBuffer();
#ifdef _WIN32
    uint32_t tid = static_cast<uint32_t>(::GetCurrentThreadId());
#else
    // XXX: Deprecated on new MACOSX
    #if __MAC_OS_X_VERSION_MIN_REQUIRED >= MAC_OS_X_VERSION_10_12
    uint64_t tid64;
    pthread_threadid_np(NULL, &tid64);
    uint32_t tid = static_cast<uint32_t>(tid64);
    #else
    uint32_t tid = static_cast<uint32_t>(::syscall(SYS_gettid));
    #endif
#endif
    fmtlog::threadBuffer->nameSize =
      fmt::format_to_n(fmtlog::threadBuffer->name, sizeof(fmtlog::threadBuffer->name), "{}", tid).size;
    sbc.threadBufferCreated();

    std::unique_lock<std::mutex> guard(bufferMutex);
    threadBuffers.push_back(fmtlog::threadBuffer);
  }

  template<size_t I, typename T>
  inline void setArg(const T& arg) {
    args[reorderIdx[I]] = arg;
  }

  template<size_t I, typename T>
  inline void setArgVal(const T& arg) {
    fmt::detail::value<fmtlog::Context>& value_ = *(fmt::detail::value<fmtlog::Context>*)&args[reorderIdx[I]];
    value_ = arg;
  }

  void flushLogFile() {
    if (outputFp) {
      fwrite(membuf.data(), 1, membuf.size(), outputFp);
      if (!manageFp) fflush(outputFp);
      else
        fpos += membuf.size();
    }
    membuf.clear();
    nextFlushTime = (std::numeric_limits<int64_t>::max)();
  }

  void closeLogFile() {
    if (membuf.size()) flushLogFile();
    if (manageFp) fclose(outputFp);
    outputFp = nullptr;
    manageFp = false;
  }

  void startPollingThread(int64_t pollInterval) {
    stopPollingThread();
    threadRunning = true;
    thr = std::thread([pollInterval, this]() {
      while (threadRunning) {
        int64_t before = fmtlogWrapper<>::impl.tscns.rdns();
        poll(false);
        int64_t delay = fmtlogWrapper<>::impl.tscns.rdns() - before;
        if (delay < pollInterval) {
          std::this_thread::sleep_for(std::chrono::nanoseconds(pollInterval - delay));
        }
      }
      poll(true);
    });
  }

  void stopPollingThread() {
    if (!threadRunning) return;
    threadRunning = false;
    if (thr.joinable()) thr.join();
  }

  void handleLog(fmt::string_view threadName, const fmtlog::SPSCVarQueueOPT::MsgHeader* header) {
    setArgVal<6>(threadName);
    StaticLogInfo& info = bgLogInfos[header->logId];
    const char* data = (const char*)(header + 1);
    const char* end = (const char*)header + header->size;
    int64_t tsc = *(int64_t*)data;
    data += 8;
    if (!info.formatToFn) { // log once
      info.location = *(const char**)data;
      data += 8;
      info.processLocation();
    }
    int64_t ts = fmtlogWrapper<>::impl.tscns.tsc2ns(tsc);
    // the date could go back when polling different threads
    uint64_t t = (ts > midnightNs) ? (ts - midnightNs) : 0;
    nanosecond.fromi(t % 1000000000);
    t /= 1000000000;
    second.fromi(t % 60);
    t /= 60;
    minute.fromi(t % 60);
    t /= 60;
    uint32_t h = t; // hour
    if (h > 23) {
      h %= 24;
      resetDate();
    }
    hour.fromi(h);
    setArgVal<14>(info.getBase());
    setArgVal<15>(info.getLocation());
    logLevel = (const char*)"DBG INF WRN ERR OFF" + (info.logLevel << 2);

    size_t headerPos = membuf.size();
    fmtlog::vformat_to(membuf, headerPattern, fmt::basic_format_args(args.data(), parttenArgSize));
    size_t bodyPos = membuf.size();

    if (info.formatToFn) {
      info.formatToFn(info.formatString, data, membuf, info.argIdx, args);
    }
    else { // log once
      membuf.append(fmt::string_view(data, end - data));
    }

    if (logCB && info.logLevel >= minCBLogLevel) {
      logCB(ts, info.logLevel, info.getLocation(), info.basePos, threadName,
            fmt::string_view(membuf.data() + headerPos, membuf.size() - headerPos), bodyPos - headerPos,
            fpos + headerPos);
    }
    membuf.push_back('\n');
    if (membuf.size() >= flushBufSize || info.logLevel >= flushLogLevel) {
      flushLogFile();
    }
  }

  void adjustHeap(size_t i) {
    while (true) {
      size_t min_i = i;
      for (size_t ch = i * 2 + 1, end = std::min(ch + 2, bgThreadBuffers.size()); ch < end; ch++) {
        auto h_ch = bgThreadBuffers[ch].header;
        auto h_min = bgThreadBuffers[min_i].header;
        if (h_ch && (!h_min || *(int64_t*)(h_ch + 1) < *(int64_t*)(h_min + 1))) min_i = ch;
      }
      if (min_i == i) break;
      std::swap(bgThreadBuffers[i], bgThreadBuffers[min_i]);
      i = min_i;
    }
  }

  void poll(bool forceFlush) {
    fmtlogWrapper<>::impl.tscns.calibrate();
    int64_t tsc = fmtlogWrapper<>::impl.tscns.rdtsc();
    if (logInfos.size()) {
      std::unique_lock<std::mutex> lock(logInfoMutex);
      for (auto& info : logInfos) {
        info.processLocation();
      }
      bgLogInfos.insert(bgLogInfos.end(), logInfos.begin(), logInfos.end());
      logInfos.clear();
    }
    if (threadBuffers.size()) {
      std::unique_lock<std::mutex> lock(bufferMutex);
      for (auto tb : threadBuffers) {
        bgThreadBuffers.emplace_back(tb);
      }
      threadBuffers.clear();
    }

    for (size_t i = 0; i < bgThreadBuffers.size(); i++) {
      auto& node = bgThreadBuffers[i];
      if (node.header) continue;
      node.header = node.tb->varq.front();
      if (!node.header && node.tb->shouldDeallocate) {
        delete node.tb;
        node = bgThreadBuffers.back();
        bgThreadBuffers.pop_back();
        i--;
      }
    }

    if (bgThreadBuffers.empty()) return;

    // build heap
    for (int i = bgThreadBuffers.size() / 2; i >= 0; i--) {
      adjustHeap(i);
    }

    while (true) {
      auto h = bgThreadBuffers[0].header;
      if (!h || h->logId >= bgLogInfos.size() || *(int64_t*)(h + 1) >= tsc) break;
      auto tb = bgThreadBuffers[0].tb;
      handleLog(fmt::string_view(tb->name, tb->nameSize), h);
      tb->varq.pop();
      bgThreadBuffers[0].header = tb->varq.front();
      adjustHeap(0);
    }

    if (membuf.size() == 0) return;
    if (!manageFp || forceFlush) {
      flushLogFile();
      return;
    }
    int64_t now = fmtlogWrapper<>::impl.tscns.tsc2ns(tsc);
    if (now > nextFlushTime) {
      flushLogFile();
    }
    else if (nextFlushTime == (std::numeric_limits<int64_t>::max)()) {
      nextFlushTime = now + flushDelay;
    }
  }
};

template<int _>
thread_local typename fmtlogDetailT<_>::ThreadBufferDestroyer fmtlogDetailT<_>::sbc;

template<int __ = 0>
struct fmtlogDetailWrapper
{ static fmtlogDetailT<> impl; };

template<int _>
fmtlogDetailT<> fmtlogDetailWrapper<_>::impl;

template<int _>
void fmtlogT<_>::registerLogInfo(uint32_t& logId, FormatToFn fn, const char* location,
                                 LogLevel level, fmt::string_view fmtString) noexcept {
  auto& d = fmtlogDetailWrapper<>::impl;
  std::lock_guard<std::mutex> lock(d.logInfoMutex);
  if (logId) return;
  logId = d.logInfos.size() + d.bgLogInfos.size();
  d.logInfos.emplace_back(fn, location, level, fmtString);
}

template<int _>
void fmtlogT<_>::vformat_to(fmtlog::MemoryBuffer& out, fmt::string_view fmt,
                            fmt::format_args args) {
  fmt::detail::vformat_to(out, fmt, args);
}

template<int _>
size_t fmtlogT<_>::formatted_size(fmt::string_view fmt, fmt::format_args args) {
  auto buf = fmt::detail::counting_buffer<>();
  fmt::detail::vformat_to(buf, fmt, args);
  return buf.count();
}

template<int _>
void fmtlogT<_>::vformat_to(char* out, fmt::string_view fmt, fmt::format_args args) {
  fmt::vformat_to(out, fmt, args);
}

template<int _>
typename fmtlogT<_>::SPSCVarQueueOPT::MsgHeader* fmtlogT<_>::allocMsg(uint32_t size,
                                                                      bool q_full_cb) noexcept {
  auto& d = fmtlogDetailWrapper<>::impl;
  if (threadBuffer == nullptr) preallocate();
  auto ret = threadBuffer->varq.alloc(size);
  if ((ret == nullptr) & q_full_cb) d.logQFullCB(d.logQFullCBArg);
  return ret;
}

template<int _>
typename fmtlogT<_>::SPSCVarQueueOPT::MsgHeader*
fmtlogT<_>::SPSCVarQueueOPT::allocMsg(uint32_t size) noexcept {
  return alloc(size);
}

template<int _>
void fmtlogT<_>::preallocate() noexcept {
  fmtlogDetailWrapper<>::impl.preallocate();
}

template<int _>
void fmtlogT<_>::setLogFile(const char* filename, bool truncate) {
  auto& d = fmtlogDetailWrapper<>::impl;
  FILE* newFp = fopen(filename, truncate ? "w" : "a");
  if (!newFp) {
    std::string err = fmt::format("Unable to open file: {}: {}", filename, strerror(errno));
    fmt::report_error(err.c_str());
  }
  setbuf(newFp, nullptr);
  d.fpos = ftell(newFp);

  closeLogFile();
  d.outputFp = newFp;
  d.manageFp = true;
}

template<int _>
void fmtlogT<_>::setLogFile(FILE* fp, bool manageFp) {
  auto& d = fmtlogDetailWrapper<>::impl;
  closeLogFile();
  if (manageFp) {
    setbuf(fp, nullptr);
    d.fpos = ftell(fp);
  }
  else
    d.fpos = 0;
  d.outputFp = fp;
  d.manageFp = manageFp;
}

template<int _>
void fmtlogT<_>::setFlushDelay(int64_t ns) noexcept {
  fmtlogDetailWrapper<>::impl.flushDelay = ns;
}

template<int _>
void fmtlogT<_>::flushOn(LogLevel flushLogLevel) noexcept {
  fmtlogDetailWrapper<>::impl.flushLogLevel = flushLogLevel;
}

template<int _>
void fmtlogT<_>::setFlushBufSize(uint32_t bytes) noexcept {
  fmtlogDetailWrapper<>::impl.flushBufSize = bytes;
}

template<int _>
void fmtlogT<_>::closeLogFile() noexcept {
  fmtlogDetailWrapper<>::impl.closeLogFile();
}

template<int _>
void fmtlogT<_>::poll(bool forceFlush) {
  fmtlogDetailWrapper<>::impl.poll(forceFlush);
}

template<int _>
void fmtlogT<_>::setThreadName(const char* name) noexcept {
  preallocate();
  threadBuffer->nameSize = fmt::format_to_n(threadBuffer->name, sizeof(fmtlog::threadBuffer->name), "{}", name).size;
}

template<int _>
void fmtlogT<_>::setLogCB(LogCBFn cb, LogLevel minCBLogLevel_) noexcept {
  auto& d = fmtlogDetailWrapper<>::impl;
  d.logCB = cb;
  d.minCBLogLevel = minCBLogLevel_;
}

template<int _>
void fmtlogT<_>::setLogQFullCB(LogQFullCBFn cb, void* userData) noexcept {
  auto& d = fmtlogDetailWrapper<>::impl;
  d.logQFullCB = cb;
  d.logQFullCBArg = userData;
}

template<int _>
void fmtlogT<_>::setHeaderPattern(const char* pattern) {
  fmtlogDetailWrapper<>::impl.setHeaderPattern(pattern);
}

template<int _>
void fmtlogT<_>::startPollingThread(int64_t pollInterval) noexcept {
  fmtlogDetailWrapper<>::impl.startPollingThread(pollInterval);
}

template<int _>
void fmtlogT<_>::stopPollingThread() noexcept {
  fmtlogDetailWrapper<>::impl.stopPollingThread();
}

template class fmtlogT<0>;



export {
namespace fmt {
  
// template<typename T>
// struct default_serializer_format {
//   static constexpr bool value = false;
// };

template<typename T>
requires (
  //default_serializer_format<T>::value && 
  requires (const T& value) {
     { rfl::enum_to_string(value) } -> std::convertible_to<std::string>;
  }
)
struct formatter<T>: formatter<std::string> {
    template <typename FormatContext>
    auto format(const T& value, FormatContext& ctx) const {
      return formatter<std::string>::format(rfl::enum_to_string(value), ctx);
    }
};


template<typename T>
requires (
  //default_serializer_format<T>::value && 
  requires (const T& value) {
     { value.to_debug_string() } -> std::convertible_to<std::string>;
  }
)
struct formatter<T>: formatter<std::string> {
    template <typename FormatContext>
    auto format(const T& value, FormatContext& ctx) const {
      return formatter<std::string>::format(value.to_debug_string(), ctx);
    }
};

}
}

//fmt::format(
export bool enable_logging = false;

export inline void log_configure(const bool is_testing_mode = false) {
  if (is_testing_mode) {
    enable_logging = true;
    fmtlog::setLogLevel(fmtlog::DBG);
    fmtlog::setThreadName("test-main");
    fmtlog::setFlushDelay(0);
    fmtlog::setLogFile(stderr, false);
  } else {
  #ifndef NO_POSTGRES
    fmtlog::setLogFile("/Users/styczynski/Code/bioseqdb/main/loglog.log", false);
    fmtlog::setFlushDelay(0);
  #endif
    fmtlog::setLogLevel(fmtlog::DBG);
    fmtlog::setThreadName("main");
  }
}

//export namespace fmto = fmt;

// template <typename>
// struct pg_node_formatter {};

// template <typename FormatContext>
// auto format(T obj, FormatContext& ctx) {
//   return format(&obj);
// }

