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

/**
 * @file core.cppm
 * @brief Defines the embedded fmt API and asynchronous logging queue.
 */
module;

// clang-format off
// #define FMT_HEADER_ONLY
#define FMT_ATTACH_TO_GLOBAL_MODULE
#include <atomic>
#include <chrono>
#include <concepts>
#include <limits>
#include <ios>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

// fmt configuration and headers.
#define FMT_MODULE

#ifdef _MSVC_LANG
#define FMT_CPLUSPLUS _MSVC_LANG
#else
#define FMT_CPLUSPLUS __cplusplus
#endif

// Put all implementation-provided headers into the global module fragment
// to prevent attachment to this module.
#ifndef FMT_IMPORT_STD
#include <algorithm>
#include <bitset>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#if FMT_CPLUSPLUS > 202002L
#include <expected>
#endif
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <locale>
#include <memory>
#include <optional>
#include <ostream>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>
#include <typeinfo>
#include <utility>
#include <variant>
#include <vector>
#else
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#endif
#include <cerrno>
#include <climits>
#include <version>

#if __has_include(<cxxabi.h>)
#include <cxxabi.h>
#endif
#if defined(_MSC_VER) || defined(__MINGW32__)
#include <intrin.h>
#endif
#if defined __APPLE__ || defined(__FreeBSD__)
#include <xlocale.h>
#endif
#if __has_include(<winapifamily.h>)
#include <winapifamily.h>
#endif
#if (__has_include(<fcntl.h>) || defined(__APPLE__) || \
     defined(__linux__)) &&                            \
    (!defined(WINAPI_FAMILY) || (WINAPI_FAMILY == WINAPI_FAMILY_DESKTOP_APP))
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifndef _WIN32
#include <unistd.h>
#else
#include <io.h>
#endif
#endif
#if defined(__APPLE__)
#include <AvailabilityMacros.h>
#include <pthread.h>
#endif
#ifdef _WIN32
#if defined(__GLIBCXX__)
#include <ext/stdio_filebuf.h>
#include <ext/stdio_sync_filebuf.h>
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

// End of fmt headers.

#ifdef _MSC_VER
#include <intrin.h>
#endif

#ifdef _WIN32
#define FAST_THREAD_LOCAL thread_local
#else
#define FAST_THREAD_LOCAL __thread
#endif

// Set FMTLOG_BLOCK to 1 to wait for queue space when a record fits.
// Oversized records are dropped even in blocking mode.
#ifndef FMTLOG_BLOCK
#define FMTLOG_BLOCK 0
#endif

#ifndef FMTLOG_QUEUE_SIZE
#define FMTLOG_QUEUE_SIZE (1 << 20)
#endif
// clang-format on

export module bioseq.common.logging:fmtlog_backend_core;

// Export the fmt API.

#ifdef FMT_IMPORT_STD
import std;
#endif

#define FMT_EXPORT export
#define FMT_BEGIN_EXPORT export {
#define FMT_END_EXPORT }

// FMT_ATTACH_TO_GLOBAL_MODULE gives fmt declarations their usual linkage.
// This lets imported fmt code link with translation units that include fmt headers.
#ifdef FMT_ATTACH_TO_GLOBAL_MODULE
extern "C++" {
#endif

#ifndef FMT_OS
#define FMT_OS 1
#endif

// All library-provided declarations and definitions must be in the module
// purview to be exported.
#include "fmt/args.h"
#include "fmt/chrono.h"
#include "fmt/color.h"
#include "fmt/compile.h"
#include "fmt/format.h"
#if FMT_OS
#include "fmt/os.h"
#endif
#include "fmt/ostream.h"
#include "fmt/printf.h"
#include "fmt/ranges.h"
// fmt/std.h only applies FMT_EXPORT to formatter specializations. Those
// specializations are attached to the exported formatter primary already, and
// GCC rejects spelling them as unbraced `export template ...` declarations.
#undef FMT_EXPORT
#define FMT_EXPORT
#include "fmt/std.h"
#undef FMT_EXPORT
#define FMT_EXPORT export
#include "fmt/xchar.h"

#ifdef FMT_ATTACH_TO_GLOBAL_MODULE
}
#endif

// End of fmt exports.

/** @brief Internal helpers for decoding queue-stored format arguments. */
namespace fmtlogdetail {
/** @brief Selects pointer arguments whose pointees are formatted after decoding. */
template <typename Arg> struct UnrefPtr : std::false_type {
    using type = Arg;
};

/** @brief Formats char pointers as strings without dereferencing one character. */
template <> struct UnrefPtr<char*> : std::false_type {
    using type = char*;
};

/** @brief Raw @c void* arguments are stored by address and are not dereferenced. */
template <> struct UnrefPtr<void*> : std::false_type {
    using type = void*;
};

/** @brief Shared-pointer arguments decode to the pointee type. */
template <typename Arg> struct UnrefPtr<std::shared_ptr<Arg>> : std::true_type {
    using type = Arg;
};

/** @brief Unique-pointer arguments decode to the pointee type. */
template <typename Arg, typename D> struct UnrefPtr<std::unique_ptr<Arg, D>> : std::true_type {
    using type = Arg;
};

/** @brief Pointer arguments decode to the pointee type. */
template <typename Arg> struct UnrefPtr<Arg*> : std::true_type {
    using type = Arg;
};

}; // namespace fmtlogdetail

/**
 * @brief Queues log arguments on each thread and formats them in one poller.
 *
 * Each thread produces records for its own queue. One consumer drains
 * those queues, formats messages with fmt and writes the output.
 *
 * @tparam __ Instance tag. The logging wrapper uses the default.
 */
template <int __ = 0> class fmtlogT {
  public:
    /** @brief Severity filter for queued records and flush policy. */
    enum LogLevel : uint8_t { DBG = 0, INF, WRN, ERR, OFF };

    
    /** @brief Allocates the calling thread's queue buffer before the first log call. */
    static void preallocate() noexcept;

    
    /** @brief Opens a log file, appending by default or truncating when requested. */
    static void setLogFile(const char* filename, bool truncate = false);

    
    /**
     * @brief Writes logs to an existing FILE handle.
     *
     * With manageFp enabled, the logger buffers output and closes the handle.
     * Otherwise each poll flushes output and the caller keeps ownership.
     */
    static void setLogFile(FILE* fp, bool manageFp = false);

    
    /**
     * @brief Drains thread queues and writes formatted records.
     *
     * Call repeatedly when no background polling thread is running. Set
     * forceFlush to bypass the usual file-buffer flush delay.
     */
    static void poll(bool forceFlush = false);

    
    /** @brief Sets the delay before a later poll flushes buffered output, in nanoseconds. */
    static void setFlushDelay(int64_t ns) noexcept;

    
    /** @brief Flushes when a record at or above @p flushLogLevel is written. */
    static void flushOn(LogLevel flushLogLevel) noexcept;

    
    /** @brief Flushes when the output buffer reaches at least bytes. */
    static void setFlushBufSize(uint32_t bytes) noexcept;

    
    /**
     * @brief Receives a formatted record whose severity passes the callback filter.
     * @param ns Timestamp in nanoseconds since the epoch.
     * @param level Record severity.
     * @param location Source path and line, possibly truncated to 255 bytes.
     * @param basePos Offset of the basename in location.
     * @param threadName Thread label set by setThreadName, or its thread ID.
     * @param msg Header and body, without the final newline.
     * @param bodyPos Offset of the body in msg.
     * @param logFilePos Record offset in a managed log file.
     */
    typedef void (*LogCBFn)(int64_t ns,
                            LogLevel level,
                            fmt::string_view location,
                            size_t basePos,
                            fmt::string_view threadName,
                            fmt::string_view msg,
                            size_t bodyPos,
                            size_t logFilePos);

    
    /** @brief Registers @p cb for records at or above @p minCBLogLevel. */
    static void setLogCB(LogCBFn cb, LogLevel minCBLogLevel) noexcept;

    /** @brief Callback invoked when a thread queue cannot accept a record. */
    typedef void (*LogQFullCBFn)(void* userData);
    /** @brief Registers the queue-full callback and opaque @p userData passed to it. */
    static void setLogQFullCB(LogQFullCBFn cb, void* userData) noexcept;

    
    /** @brief Closes the log file. Callbacks still receive subsequent records. */
    static void closeLogFile() noexcept;

    
    /** @brief Sets the fmt pattern for the line header written before each body. */
    static void setHeaderPattern(const char* pattern);

    
    /** @brief Sets the calling thread's label for the {t} header field. */
    static void setThreadName(const char* name) noexcept;

    
    /** @brief Sets the minimum level accepted by @ref checkLogLevel. */
    static inline void setLogLevel(LogLevel logLevel) noexcept;

    
    /** @brief Returns the active minimum log level. */
    static inline LogLevel getLogLevel() noexcept;

    
    /** @brief Checks the level filter, or returns true if FMTLOG_NO_CHECK_LEVEL is set. */
    static inline bool checkLogLevel(LogLevel logLevel) noexcept;

    
    /**
     * @brief Starts a thread that polls at the requested nanosecond interval.
     *
     * Do not call poll() concurrently while this thread is running.
     */
    static void startPollingThread(int64_t pollInterval = 1000000000) noexcept;

    
    /** @brief Stops the background polling thread started by @ref startPollingThread. */
    static void stopPollingThread() noexcept;

    // https://github.com/MengRao/SPSC_Queue
    /** @brief Single-producer single-consumer variable-size ring buffer for log records. */
    class SPSCVarQueueOPT {
      public:
        /** @brief Header prefix for one variable-length queue slot. */
        struct MsgHeader {
            /** @brief Publishes @p sz payload bytes after this header to the consumer. */
            inline void push(uint32_t sz) {
                *(volatile uint32_t*)&size = sz + sizeof(MsgHeader);
            }

            /** @brief Total byte length of the slot including this header. */
            uint32_t size;
            /** @brief Log-site ID, or severity for a one-shot preformatted message. */
            uint32_t logId;
        };
        /** @brief Number of header blocks in the ring, derived from @c FMTLOG_QUEUE_SIZE. */
        static constexpr uint32_t BLK_CNT = FMTLOG_QUEUE_SIZE / sizeof(MsgHeader);
        static_assert(BLK_CNT > 2,
                      "the logging queue needs a record block, a payload block, and a sentinel");

        // alloc() keeps one MsgHeader block free for the queue sentinel. The
        // returned record consumes one further block for its own header, leaving
        // this exact maximum for the caller-owned payload.
        /** @brief Maximum caller payload bytes excluding the record header block. */
        static constexpr uint32_t MAX_PAYLOAD_SIZE = (BLK_CNT - 2) * sizeof(MsgHeader);
        static_assert(MAX_PAYLOAD_SIZE < std::numeric_limits<uint32_t>::max());
        /** @brief Payload size that exceeds @ref MAX_PAYLOAD_SIZE and triggers the oversize path. */
        static constexpr uint32_t FIRST_UNSUPPORTED_PAYLOAD_SIZE = MAX_PAYLOAD_SIZE + 1;

        /** @brief Reserves a queue slot for @p size payload bytes. Returns null when full or oversize. */
        MsgHeader* allocMsg(uint32_t size) noexcept;

        /** @brief Reserves space for a record without publishing it to the consumer. */
        MsgHeader* alloc(uint32_t size) {
            // Check the payload limit before adding the header to avoid overflow.
            if (size > MAX_PAYLOAD_SIZE)
                return nullptr;
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
                } else {
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

        /** @brief Returns the next complete record, or null when the queue is empty. */
        inline const MsgHeader* front() {
            uint32_t size = blk[read_idx].size;
            if (size == 1) { // wrap around
                read_idx = 0;
                size = blk[0].size;
            }
            if (size == 0)
                return nullptr;
            return &blk[read_idx];
        }

        /** @brief Releases the record returned by @ref front. */
        inline void pop() {
            uint32_t blk_sz = (blk[read_idx].size + sizeof(MsgHeader) - 1) / sizeof(MsgHeader);
            *(volatile uint32_t*)&read_idx = read_idx + blk_sz;
        }

      private:
        /** @brief Ring storage aligned for producer cache-line isolation. */
        alignas(64) MsgHeader blk[BLK_CNT] = {};
        /** @brief Producer write index in header blocks. */
        uint32_t write_idx = 0;
        /** @brief Contiguous free blocks remaining from @c write_idx. */
        uint32_t free_write_cnt = BLK_CNT;

        /** @brief Consumer read index in header blocks, on a separate cache line. */
        alignas(128) uint32_t read_idx = 0;
    };

    /** @brief Per-thread queue and metadata used by the producer side. */
    struct ThreadBuffer {
        /** @brief Variable-size queue owned by this thread. */
        SPSCVarQueueOPT varq;
        /** @brief Set at thread exit so the poller can delete the drained buffer. */
        bool shouldDeallocate = false;
        /** @brief Fixed-size thread label storage, read using nameSize. */
        char name[32];
        /** @brief Byte length of the active prefix of @c name. */
        size_t nameSize;
    };

    // https://github.com/MengRao/tscns
    /** @brief Converts processor clock ticks to calibrated nanosecond timestamps. */
    class TSCNS {
      public:
        /** @brief Nanoseconds per second, used for calibration intervals. */
        static const int64_t NsPerSec = 1000000000;

        /** @brief Samples both clocks to estimate nanoseconds per processor tick. */
        void init(int64_t init_calibrate_ns = 20000000,
                  int64_t calibrate_interval_ns = 3 * NsPerSec) {
            calibate_interval_ns_ = calibrate_interval_ns;
            int64_t base_tsc, base_ns;
            syncTime(base_tsc, base_ns);
            int64_t expire_ns = base_ns + init_calibrate_ns;
            while (rdsysns() < expire_ns)
                std::this_thread::yield();
            int64_t delayed_tsc, delayed_ns;
            syncTime(delayed_tsc, delayed_ns);
            double init_ns_per_tsc = (double)(delayed_ns - base_ns) / (delayed_tsc - base_tsc);
            saveParam(base_tsc, base_ns, 0, init_ns_per_tsc);
        }

        /** @brief Refreshes the TSC conversion when the scheduled calibration deadline elapses. */
        void calibrate() {
            if (rdtsc() < next_calibrate_tsc_)
                return;
            int64_t tsc, ns;
            syncTime(tsc, ns);
            int64_t ns_err = tsc2ns(tsc) - ns;
            if (ns_err > 1000000)
                ns_err = 1000000;
            if (ns_err < -1000000)
                ns_err = -1000000;
            double new_ns_per_tsc = ns_per_tsc_ * (1.0 - (ns_err + ns_err - base_ns_err_) /
                                                             ((tsc - base_tsc_) * ns_per_tsc_));
            saveParam(tsc, ns, ns_err, new_ns_per_tsc);
        }

        /** @brief Reads the processor time-stamp counter, or system nanoseconds off x86. */
        static inline int64_t rdtsc() {
#ifdef _MSC_VER
            return __rdtsc();
#elif defined(__i386__) || defined(__x86_64__) || defined(__amd64__)
            return __builtin_ia32_rdtsc();
#else
            return rdsysns();
#endif
        }

        /** @brief Converts @p tsc to nanoseconds using the current calibration snapshot. */
        inline int64_t tsc2ns(int64_t tsc) const {
            while (true) {
                uint32_t before_seq = param_seq_.load(std::memory_order_acquire) & ~1;
                std::atomic_signal_fence(std::memory_order_acq_rel);
                int64_t ns = base_ns_ + (int64_t)((tsc - base_tsc_) * ns_per_tsc_);
                std::atomic_signal_fence(std::memory_order_acq_rel);
                uint32_t after_seq = param_seq_.load(std::memory_order_acquire);
                if (before_seq == after_seq)
                    return ns;
            }
        }

        /** @brief Returns the current time in nanoseconds from TSC conversion. */
        inline int64_t rdns() const {
            return tsc2ns(rdtsc());
        }

        /** @brief Reads system-clock epoch time in nanoseconds. */
        static inline int64_t rdsysns() {
            using namespace std::chrono;
            return duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();
        }

        /** @brief Returns effective TSC frequency in gigahertz from the current calibration. */
        double getTscGhz() const {
            return 1.0 / ns_per_tsc_;
        }

        // Choose the system-clock sample bracketed by the closest processor ticks.
        // On MSVC, merge equal timestamps and skip incomplete edge intervals
        // because the system clock has coarser resolution.
        /** @brief Pairs a TSC sample with a system-clock nanosecond sample for calibration. */
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
                if (ns[i] == ns[i - 1])
                    continue;
                tsc[j - 1] = tsc[i - 1];
                ns[j++] = ns[i];
            }
            j--;
#else
            int j = N + 1;
#endif

            int best = 1;
            for (int i = 2; i < j; i++) {
                if (tsc[i] - tsc[i - 1] < tsc[best] - tsc[best - 1])
                    best = i;
            }
            tsc_out = (tsc[best] + tsc[best - 1]) >> 1;
            ns_out = ns[best];
        }

        /** @brief Updates calibration while marking concurrent reads for retry. */
        void
        saveParam(int64_t base_tsc, int64_t sys_ns, int64_t base_ns_err, double new_ns_per_tsc) {
            base_ns_err_ = base_ns_err;
            next_calibrate_tsc_ =
                base_tsc + (int64_t)((calibate_interval_ns_ - 1000) / new_ns_per_tsc);
            uint32_t seq = param_seq_.load(std::memory_order_relaxed);
            param_seq_.store(++seq, std::memory_order_release);
            std::atomic_signal_fence(std::memory_order_acq_rel);
            base_tsc_ = base_tsc;
            base_ns_ = sys_ns + base_ns_err;
            ns_per_tsc_ = new_ns_per_tsc;
            std::atomic_signal_fence(std::memory_order_acq_rel);
            param_seq_.store(++seq, std::memory_order_release);
        }

        /** @brief Version counter used to detect calibration changes during a read. */
        alignas(64) std::atomic<uint32_t> param_seq_ = 0;
        /** @brief Current nanoseconds elapsed per TSC tick. */
        double ns_per_tsc_;
        /** @brief TSC value at the last published baseline. */
        int64_t base_tsc_;
        /** @brief Nanosecond epoch aligned to @c base_tsc_. */
        int64_t base_ns_;
        /** @brief Interval between automatic calibrations in nanoseconds. */
        int64_t calibate_interval_ns_;
        /** @brief Residual nanosecond error at the last baseline. */
        int64_t base_ns_err_;
        /** @brief TSC deadline for the next @ref calibrate call. */
        int64_t next_calibrate_tsc_;
    };

    /** @brief Initializes TSC calibration and sets the default minimum log level. */
    void init() {
        tscns.init();
        currentLogLevel = INF;
    }

    /** @brief fmt format context type used when decoding queued arguments. */
    using Context = fmt::format_context;
    /** @brief Formatting buffer with 10,000 inline bytes and heap growth when needed. */
    using MemoryBuffer = fmt::basic_memory_buffer<char, 10000>;
    /** @brief Deferred formatter that decodes queue bytes and appends one record to @p out. */
    typedef const char* (*FormatToFn)(fmt::string_view format,
                                      const char* data,
                                      MemoryBuffer& out,
                                      int& argIdx,
                                      std::vector<fmt::basic_format_arg<Context>>& args);

    /** @brief Registers format metadata and assigns @p logId on first use of a log site. */
    static void registerLogInfo(uint32_t& logId,
                                FormatToFn fn,
                                const char* location,
                                LogLevel level,
                                fmt::string_view fmtString) noexcept;

    /** @brief Appends a formatted string to @p out using fmt dynamic arguments. */
    static void vformat_to(MemoryBuffer& out, fmt::string_view fmt, fmt::format_args args);

    /** @brief Returns the byte length of a formatted string without writing it. */
    static size_t formatted_size(fmt::string_view fmt, fmt::format_args args);

    /** @brief Writes a formatted string into a caller-owned byte buffer. */
    static void vformat_to(char* out, fmt::string_view fmt, fmt::format_args args);

    /** @brief Allocates a queue record, optionally signaling the queue-full callback. */
    static typename SPSCVarQueueOPT::MsgHeader* allocMsg(uint32_t size, bool logQFullCB) noexcept;

    /** @brief TSC converter shared by timestamp fields in queued records. */
    TSCNS tscns;

    /** @brief Active minimum level filter, read without locking on the producer path. */
    volatile LogLevel currentLogLevel;
    /** @brief Thread-local producer buffer, lazily allocated by @ref preallocate. */
    static FAST_THREAD_LOCAL ThreadBuffer* threadBuffer;

    /** @brief Detects fmt named-argument wrappers in a log argument pack. */
    template <typename Arg> static inline constexpr bool isNamedArg() {
        return fmt::detail::is_named_arg<fmt::remove_cvref_t<Arg>>::value;
    }

    /** @brief Strips fmt named-argument wrappers to the underlying value type. */
    template <typename Arg> struct unNamedType {
        using type = Arg;
    };

    /** @brief Maps dynamic named arguments to their stored value types. */
    template <typename Arg> struct unNamedType<fmt::detail::named_arg<char, Arg>> {
        using type = Arg;
    };

#if FMT_USE_NONTYPE_TEMPLATE_ARGS
    /** @brief Maps compile-time named arguments to their stored value types. */
    template <typename Arg, size_t N, fmt::detail::fixed_string<char, N> Str>
    struct unNamedType<fmt::detail::static_named_arg<Arg, char, N, Str>> {
        using type = Arg;
    };
#endif

    /** @brief Checks whether fmt treats an argument as a C string. */
    template <typename Arg> static inline constexpr bool isCstring() {
        return fmt::detail::mapped_type_constant<Arg, char>::value ==
               fmt::detail::type::cstring_type;
    }

    /** @brief Checks whether fmt treats an argument as a string view. */
    template <typename Arg> static inline constexpr bool isString() {
        return fmt::detail::mapped_type_constant<Arg, char>::value ==
               fmt::detail::type::string_type;
    }

    /** @brief Detects C-string storage needs, including through named arguments. */
    template <typename Arg> static inline constexpr bool isStoredCstring() {
        using ArgType = fmt::remove_cvref_t<Arg>;
        if constexpr (isNamedArg<ArgType>()) {
            return isCstring<typename unNamedType<ArgType>::type>();
        }
        return isCstring<Arg>();
    }

    /** @brief Detects queue values that need explicit destruction after decode. */
    template <typename Arg> static inline constexpr bool needCallDtor() {
        using ArgType = fmt::remove_cvref_t<Arg>;
        if constexpr (isNamedArg<Arg>()) {
            return needCallDtor<typename unNamedType<ArgType>::type>();
        }
        if constexpr (isString<Arg>())
            return false;
        return !std::is_trivially_destructible<ArgType>::value;
    }

    /*
     * Strings and builtin scalar arguments are packed without padding. A scalar
     * can start at any byte after a string, so decode it with memcpy. Custom
     * formatters may keep an object's address until formatTo() finishes. These
     * objects, and objects that need a destructor, require aligned storage and
     * must stay alive in the queue until then.
     */
    /** @brief Detects argument types that require aligned object storage in the queue. */
    template <typename Arg> static inline constexpr bool needAlignedArgStorage() {
        using ArgType = fmt::remove_cvref_t<Arg>;
        return !std::is_scalar_v<ArgType> ||
               fmt::detail::mapped_type_constant<ArgType, char>::value ==
                   fmt::detail::type::custom_type;
    }

    /** @brief Advances @p ptr to the next aligned address for @c ArgType storage. */
    template <typename ArgType> static inline char* alignArgStorage(char* ptr) noexcept {
        constexpr size_t alignment = alignof(ArgType);
        const auto address = reinterpret_cast<std::uintptr_t>(ptr);
        const auto padding = (alignment - (address % alignment)) % alignment;
        return ptr + padding;
    }

    /** @brief Byte size of the TSC prefix written before encoded arguments. */
    static constexpr size_t LOG_RECORD_PREFIX_SIZE = sizeof(int64_t);
    /** @brief Maximum encoded argument bytes allowed after the TSC prefix. */
    static constexpr size_t MAX_ARGUMENT_STORAGE_SIZE =
        SPSCVarQueueOPT::MAX_PAYLOAD_SIZE - LOG_RECORD_PREFIX_SIZE;

    /** @brief Subtracts @p requested from @p remaining when the reservation fits. */
    static inline constexpr bool reserveArgStorage(size_t& remaining, size_t requested) noexcept {
        if (requested > remaining)
            return false;
        remaining -= requested;
        return true;
    }

    /** @brief Ends argument-size checking when no arguments remain. */
    template <size_t CstringIdx>
    static inline constexpr bool getArgSizes(size_t* cstringSize, size_t& remaining) {
        return true;
    }

    /** @brief Checks argument storage against remaining space and records C-string sizes. */
    template <size_t CstringIdx, typename Arg, typename... Args>
    static inline constexpr bool
    getArgSizes(size_t* cstringSize, size_t& remaining, const Arg& arg, const Args&... args) {
        if constexpr (isNamedArg<Arg>()) {
            return getArgSizes<CstringIdx>(cstringSize, remaining, arg.value, args...);
        } else if constexpr (isCstring<Arg>()) {
            const size_t len = strlen(arg);
            // The terminating NUL is part of the queue representation. Checking
            // against remaining before +1 avoids both size_t overflow and a partial
            // queue record.
            if (len >= remaining)
                return false;
            const size_t stored_size = len + 1;
            cstringSize[CstringIdx] = stored_size;
            remaining -= stored_size;
            return getArgSizes<CstringIdx + 1>(cstringSize, remaining, args...);
        } else if constexpr (isString<Arg>()) {
            const size_t len = arg.size();
            if (len >= remaining)
                return false;
            remaining -= len + 1;
            return getArgSizes<CstringIdx>(cstringSize, remaining, args...);
        } else {
            using ArgType = fmt::remove_cvref_t<Arg>;
            constexpr size_t maximum_padding =
                needAlignedArgStorage<ArgType>() ? alignof(ArgType) - 1 : 0;
            if (!reserveArgStorage(remaining, maximum_padding) ||
                !reserveArgStorage(remaining, sizeof(ArgType))) {
                return false;
            }
            return getArgSizes<CstringIdx>(cstringSize, remaining, args...);
        }
    }

    /** @brief Returns the end of the encoded arguments. */
    template <size_t CstringIdx>
    static inline constexpr char* encodeArgs(size_t* cstringSize, char* out) {
        return out;
    }

    /** @brief Serializes log arguments into the queue payload at @p out. */
    template <size_t CstringIdx, typename Arg, typename... Args>
    static inline constexpr char*
    encodeArgs(size_t* cstringSize, char* out, Arg&& arg, Args&&... args) {
        if constexpr (isNamedArg<Arg>()) {
            return encodeArgs<CstringIdx>(cstringSize, out, arg.value, std::forward<Args>(args)...);
        } else if constexpr (isCstring<Arg>()) {
            memcpy(out, arg, cstringSize[CstringIdx]);
            return encodeArgs<CstringIdx + 1>(
                cstringSize, out + cstringSize[CstringIdx], std::forward<Args>(args)...);
        } else if constexpr (isString<Arg>()) {
            size_t len = arg.size();
            memcpy(out, arg.data(), len);
            out[len] = 0;
            return encodeArgs<CstringIdx>(cstringSize, out + len + 1, std::forward<Args>(args)...);
        } else {
            using ArgType = fmt::remove_cvref_t<Arg>;
            if constexpr (needAlignedArgStorage<ArgType>()) {
                out = alignArgStorage<ArgType>(out);
            }

            // Packed builtin scalars are copied as bytes and decoded with memcpy.
            // Custom and non-scalar arguments need aligned object storage because fmt
            // may keep their address until the asynchronous formatting pass finishes.
            if constexpr (!needAlignedArgStorage<ArgType>()) {
                static_assert(std::is_scalar_v<ArgType> && std::is_trivially_copyable_v<ArgType>);
                memcpy(out, &arg, sizeof(ArgType));
            } else if constexpr (std::is_constructible_v<ArgType, Arg&&>) {
                std::construct_at(reinterpret_cast<ArgType*>(out), std::forward<Arg>(arg));
            } else {
                // Raw arrays reach this branch. Copying bytes starts their lifetime only
                // for implicit-lifetime types, which the assertions require.
                static_assert(std::is_trivially_copyable_v<ArgType>);
                static_assert(std::is_implicit_lifetime_v<ArgType>);
                memcpy(out, &arg, sizeof(ArgType));
            }
            return encodeArgs<CstringIdx>(
                cstringSize, out + sizeof(ArgType), std::forward<Args>(args)...);
        }
    }

    /** @brief Ends named-argument collection when no arguments remain. */
    template <size_t Idx, size_t NamedIdx>
    static inline constexpr void
    storeNamedArgs(fmt::detail::named_arg_info<char>* named_args_store) {}

    /** @brief Records fmt named-argument names and positional indices for format rewriting. */
    template <size_t Idx, size_t NamedIdx, typename Arg, typename... Args>
    static inline constexpr void storeNamedArgs(fmt::detail::named_arg_info<char>* named_args_store,
                                                const Arg& arg,
                                                const Args&... args) {
        if constexpr (isNamedArg<Arg>()) {
            named_args_store[NamedIdx] = {arg.name, Idx};
            storeNamedArgs<Idx + 1, NamedIdx + 1>(named_args_store, args...);
        } else {
            storeNamedArgs<Idx + 1, NamedIdx>(named_args_store, args...);
        }
    }

    /** @brief Stores a decoded argument, optionally keeping its cached fmt type. */
    template <bool ValueOnly, size_t Idx, typename Decoded>
    static inline void storeDecodedArg(fmt::basic_format_arg<Context>* args, Decoded& decoded) {
        if constexpr (ValueOnly) {
            // fmt's basic_format_arg is standard-layout and value_ is its first
            // member, so these objects are pointer-interconvertible.  Updating only
            // value_ preserves fmtlog's cached-type fast path after the first record.
            static_assert(std::is_standard_layout_v<fmt::basic_format_arg<Context>>);
            auto* value = reinterpret_cast<fmt::detail::value<Context>*>(args + Idx);
            fmt::detail::value<Context>& value_ = *value;
            value_ = decoded;
        } else {
            args[Idx] = decoded;
        }
    }

    /** @brief Returns the end of the decoded arguments. */
    template <bool ValueOnly, size_t Idx, size_t DestructIdx>
    static inline const char*
    decodeArgs(const char* in, fmt::basic_format_arg<Context>* args, const char** destruct_args) {
        return in;
    }

    /** @brief Rebuilds fmt arguments and remembers objects to destroy after formatting. */
    template <bool ValueOnly, size_t Idx, size_t DestructIdx, typename Arg, typename... Args>
    static inline const char*
    decodeArgs(const char* in, fmt::basic_format_arg<Context>* args, const char** destruct_args) {
        using namespace fmtlogdetail;
        using ArgType = fmt::remove_cvref_t<Arg>;
        if constexpr (isNamedArg<ArgType>()) {
            return decodeArgs<ValueOnly,
                              Idx,
                              DestructIdx,
                              typename unNamedType<ArgType>::type,
                              Args...>(in, args, destruct_args);
        } else if constexpr (isCstring<Arg>() || isString<Arg>()) {
            size_t size = strlen(in);
            fmt::string_view v(in, size);
            storeDecodedArg<ValueOnly, Idx>(args, v);
            return decodeArgs<ValueOnly, Idx + 1, DestructIdx, Args...>(
                in + size + 1, args, destruct_args);
        } else {
            char* encoded = const_cast<char*>(in);
            if constexpr (needAlignedArgStorage<ArgType>()) {
                encoded = alignArgStorage<ArgType>(encoded);
                auto& decoded = *std::launder(reinterpret_cast<ArgType*>(encoded));
                if constexpr (UnrefPtr<ArgType>::value) {
                    auto& unwrapped = *decoded;
                    storeDecodedArg<ValueOnly, Idx>(args, unwrapped);
                } else {
                    storeDecodedArg<ValueOnly, Idx>(args, decoded);
                }
            } else {
                // Scalar queue fields are intentionally byte-packed.  Decode through
                // an aligned local object so odd-offset fields never create a
                // misaligned pointer or reference.
                static_assert(std::is_scalar_v<ArgType> && std::is_trivially_copyable_v<ArgType>);
                ArgType decoded{};
                memcpy(&decoded, encoded, sizeof(ArgType));
                if constexpr (UnrefPtr<ArgType>::value) {
                    auto& unwrapped = *decoded;
                    storeDecodedArg<ValueOnly, Idx>(args, unwrapped);
                } else {
                    storeDecodedArg<ValueOnly, Idx>(args, decoded);
                }
            }

            if constexpr (needCallDtor<Arg>()) {
                destruct_args[DestructIdx] = encoded;
                return decodeArgs<ValueOnly, Idx + 1, DestructIdx + 1, Args...>(
                    encoded + sizeof(ArgType), args, destruct_args);
            } else {
                return decodeArgs<ValueOnly, Idx + 1, DestructIdx, Args...>(
                    encoded + sizeof(ArgType), args, destruct_args);
            }
        }
    }

    /** @brief Ends cleanup when no stored objects remain. */
    template <size_t DestructIdx> static inline void destructArgs(const char** destruct_args) {}

    /** @brief Runs destructors for queue-stored arguments that required explicit cleanup. */
    template <size_t DestructIdx, typename Arg, typename... Args>
    static inline void destructArgs(const char** destruct_args) {
        using ArgType = fmt::remove_cvref_t<Arg>;
        if constexpr (isNamedArg<ArgType>()) {
            destructArgs<DestructIdx, typename unNamedType<ArgType>::type, Args...>(destruct_args);
        } else if constexpr (needCallDtor<Arg>()) {
            auto* stored = std::launder(
                reinterpret_cast<ArgType*>(const_cast<char*>(destruct_args[DestructIdx])));
            std::destroy_at(stored);
            destructArgs<DestructIdx + 1, Args...>(destruct_args);
        } else {
            destructArgs<DestructIdx, Args...>(destruct_args);
        }
    }

    /** @brief Decodes queued arguments and formats one deferred log record into @p out. */
    template <typename... Args>
    static const char* formatTo(fmt::string_view format,
                                const char* data,
                                MemoryBuffer& out,
                                int& argIdx,
                                std::vector<fmt::basic_format_arg<Context>>& args) {
        constexpr size_t num_args = sizeof...(Args);
        constexpr size_t num_dtors = fmt::detail::count<needCallDtor<Args>()...>();
        const char* dtor_args[std::max(num_dtors, (size_t)1)];
        const char* ret;
        if (argIdx < 0) {
            argIdx = (int)args.size();
            args.resize(argIdx + num_args);
            ret = decodeArgs<false, 0, 0, Args...>(data, args.data() + argIdx, dtor_args);
        } else {
            ret = decodeArgs<true, 0, 0, Args...>(data, args.data() + argIdx, dtor_args);
        }
        vformat_to(out, format, fmt::basic_format_args(args.data() + argIdx, num_args));
        destructArgs<0, Args...>(dtor_args);

        return ret;
    }

    /** @brief Rewrites named format placeholders to positional indices, optionally recording order. */
    template <bool Reorder, typename... Args>
    static fmt::string_view
    unNameFormat(fmt::string_view in, uint32_t* reorderIdx, const Args&... args) {
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
            if (c != '{')
                continue;
            size_t copy_size = p - begin;
            memcpy(out, begin, copy_size);
            out += copy_size;
            begin = p;
            c = *p++;
            if (!c)
                fmt::report_error("invalid format string");
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
                if (id < 0)
                    fmt::report_error("invalid format string");
                if constexpr (Reorder) {
                    reorderIdx[id] = arg_idx++;
                } else {
                    out = fmt::format_to(out, "{}", id);
                }
            } else {
                *out++ = c;
            }
            begin = p;
        }
        const char* ptr = unnamed_str.release();
        return fmt::string_view(ptr, out - ptr);
    }

  public:
    /**
     * @brief Queues arguments for the polling thread to format later.
     *
     * Registers the log site on first use. String contents are copied into
     * the queue, and other arguments are copied or moved as their types allow.
     * Records too large for the queue are dropped after the queue-full callback.
     */
    template <typename... Args>
    inline void log(uint32_t& logId,
                    int64_t tsc,
                    const char* location,
                    LogLevel level,
                    fmt::format_string<std::type_identity_t<Args>...> format,
                    Args&&... args) noexcept {
        if (!logId) {
            auto unnamed_format = unNameFormat<false>(fmt::string_view(format), nullptr, args...);
            registerLogInfo(logId, formatTo<Args...>, location, level, unnamed_format);
        }
        constexpr size_t num_cstring = fmt::detail::count<isStoredCstring<Args>()...>();
        size_t cstringSizes[std::max(num_cstring, (size_t)1)];
        size_t remaining = MAX_ARGUMENT_STORAGE_SIZE;
        if (!getArgSizes<0>(cstringSizes, remaining, args...)) {
            // Report an oversized record once. Waiting for queue space cannot
            // make it fit, even when FMTLOG_BLOCK is enabled.
            (void)allocMsg(SPSCVarQueueOPT::FIRST_UNSUPPORTED_PAYLOAD_SIZE, true);
            return;
        }
        const size_t arg_size = MAX_ARGUMENT_STORAGE_SIZE - remaining;
        const uint32_t alloc_size = static_cast<uint32_t>(LOG_RECORD_PREFIX_SIZE + arg_size);
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

    /** @brief Enqueues a one-shot preformatted message without registering a log site id. */
    template <typename... Args>
    inline void logOnce(const char* location,
                        LogLevel level,
                        fmt::format_string<Args...> format,
                        Args&&... args) {
        fmt::string_view sv(format);
        auto&& fmt_args = fmt::make_format_args(args...);
        const size_t fmt_size = formatted_size(sv, fmt_args);
        constexpr size_t fixed_size = sizeof(int64_t) + sizeof(const char*);
        if (fmt_size > SPSCVarQueueOPT::MAX_PAYLOAD_SIZE - fixed_size) {
            (void)allocMsg(SPSCVarQueueOPT::FIRST_UNSUPPORTED_PAYLOAD_SIZE, true);
            return;
        }
        const uint32_t alloc_size = static_cast<uint32_t>(fixed_size + fmt_size);
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

/** @brief Public type alias for the embedded fmtlog API. */
export using fmtlog = fmtlogT<>;

/** @brief Thread-local pointer to the active producer buffer for this fmtlog instance. */
template <int _> FAST_THREAD_LOCAL typename fmtlogT<_>::ThreadBuffer* fmtlogT<_>::threadBuffer;

/**
 * @brief Owns the process-wide embedded fmtlog instance.
 *
 * @tparam p_instance_tag Compile-time identity tag. The logging module uses the default.
 */
export template <int p_instance_tag = 0> struct fmtlog_wrapper {
    /** Process-wide asynchronous logger state. */
    static fmtlog instance;
};

/** @brief Sets the shared wrapper instance minimum accepted log level. */
template <int _> inline void fmtlogT<_>::setLogLevel(LogLevel logLevel) noexcept {
    fmtlog_wrapper<>::instance.currentLogLevel = logLevel;
}

/** @brief Returns the shared wrapper instance minimum accepted log level. */
template <int _> inline typename fmtlogT<_>::LogLevel fmtlogT<_>::getLogLevel() noexcept {
    return fmtlog_wrapper<>::instance.currentLogLevel;
}

/** @brief Applies the shared level filter unless FMTLOG_NO_CHECK_LEVEL disables it. */
template <int _> inline bool fmtlogT<_>::checkLogLevel(LogLevel logLevel) noexcept {
#ifdef FMTLOG_NO_CHECK_LEVEL
    return true;
#else
    return logLevel >= fmtlog_wrapper<>::instance.currentLogLevel;
#endif
}
