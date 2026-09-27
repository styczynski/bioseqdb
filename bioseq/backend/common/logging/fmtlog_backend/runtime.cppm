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
 * @file runtime.cppm
 * @brief Drains fmtlog queues and writes timestamped messages to files and callbacks.
 */
module;

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// clang-format off
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
// clang-format on

module bioseq.common.logging:fmtlog_backend_runtime;

import :fmtlog_backend_core;

template <int p_instance_tag> fmtlog fmtlog_wrapper<p_instance_tag>::instance;

/** No-op default handler when a per-thread log queue is full. */
inline void fmtlogEmptyFun(void*) {}

/**
 * Formats queued records, buffers output and manages the polling thread.
 *
 * @tparam ___ Instance tag. The logger uses the default runtime.
 */
template <int ___ = 0> class fmtlogDetailT {
  public:
    // https://github.com/MengRao/str
    /**
     * Fixed-size character array for decimal digits and short header fields.
     *
     * @tparam SIZE Number of stored characters.
     */
    template <size_t SIZE> class Str {
      public:
        /** Character capacity of this buffer. */
        static const int Size = SIZE;
        /** Fixed-width characters with no terminating NUL. */
        char s[SIZE];

        /** Leaves the character array uninitialized. */
        Str() {}
        /** Copies SIZE characters from the supplied address. */
        Str(const char* p) {
            *this = *(const Str<SIZE>*)p;
        }

        /** Returns the mutable character at @p i. */
        char& operator[](int i) {
            return s[i];
        }
        /** Returns the character at @p i. */
        char operator[](int i) const {
            return s[i];
        }

        /** Writes a nonnegative number as zero-padded digits, keeping the lowest SIZE digits. */
        template <typename T> void fromi(T num) {
            if constexpr (Size & 1) {
                s[Size - 1] = '0' + (num % 10);
                num /= 10;
            }
            switch (Size & -2) {
            case 18:
                *(uint16_t*)(s + 16) = *(uint16_t*)(digit_pairs + ((num % 100) << 1));
                num /= 100;
            case 16:
                *(uint16_t*)(s + 14) = *(uint16_t*)(digit_pairs + ((num % 100) << 1));
                num /= 100;
            case 14:
                *(uint16_t*)(s + 12) = *(uint16_t*)(digit_pairs + ((num % 100) << 1));
                num /= 100;
            case 12:
                *(uint16_t*)(s + 10) = *(uint16_t*)(digit_pairs + ((num % 100) << 1));
                num /= 100;
            case 10:
                *(uint16_t*)(s + 8) = *(uint16_t*)(digit_pairs + ((num % 100) << 1));
                num /= 100;
            case 8:
                *(uint16_t*)(s + 6) = *(uint16_t*)(digit_pairs + ((num % 100) << 1));
                num /= 100;
            case 6:
                *(uint16_t*)(s + 4) = *(uint16_t*)(digit_pairs + ((num % 100) << 1));
                num /= 100;
            case 4:
                *(uint16_t*)(s + 2) = *(uint16_t*)(digit_pairs + ((num % 100) << 1));
                num /= 100;
            case 2:
                *(uint16_t*)(s + 0) = *(uint16_t*)(digit_pairs + ((num % 100) << 1));
                num /= 100;
            }
        }

        /** Lookup table of two-digit decimal pairs used by @ref fromi. */
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

    /** Initializes default header pattern, buffers, and log-site tables. */
    fmtlogDetailT() : flushDelay(3000000000) {
        args.reserve(4096);
        args.resize(parttenArgSize);

        fmtlog_wrapper<>::instance.init();
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

    /** Stops polling, drains pending records, and closes the log file. */
    ~fmtlogDetailT() {
        stopPollingThread();
        poll(true);
        closeLogFile();
    }

    /** Parses @p pattern and binds reusable header format arguments. */
    void setHeaderPattern(const char* pattern) {
        if (shouldDeallocateHeader)
            delete[] headerPattern.data();
        using namespace fmt::literals;
        for (int i = 0; i < parttenArgSize; i++) {
            reorderIdx[i] = parttenArgSize - 1;
        }
        headerPattern = fmtlog::unNameFormat<true>(pattern,
                                                   reorderIdx,
                                                   "a"_a = "",
                                                   "b"_a = "",
                                                   "C"_a = "",
                                                   "Y"_a = "",
                                                   "m"_a = "",
                                                   "d"_a = "",
                                                   "t"_a = "thread name",
                                                   "F"_a = "",
                                                   "f"_a = "",
                                                   "e"_a = "",
                                                   "S"_a = "",
                                                   "M"_a = "",
                                                   "H"_a = "",
                                                   "l"_a = fmtlog::LogLevel(),
                                                   "s"_a = "fmtlog.cc:123",
                                                   "g"_a = "/home/raomeng/fmtlog/fmtlog.cc:123",
                                                   "Ymd"_a = "",
                                                   "HMS"_a = "",
                                                   "HMSe"_a = "",
                                                   "HMSf"_a = "",
                                                   "HMSF"_a = "",
                                                   "YmdHMS"_a = "",
                                                   "YmdHMSe"_a = "",
                                                   "YmdHMSf"_a = "",
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
        setArg<21>(fmt::string_view(year.s, 19)); // YmdHMS
        setArg<22>(fmt::string_view(year.s, 23)); // YmdHMSe
        setArg<23>(fmt::string_view(year.s, 26)); // YmdHMSf
        setArg<24>(fmt::string_view(year.s, 29)); // YmdHMSF
    }

    /** Marks the thread-local queue for deferred teardown when the thread exits. */
    class ThreadBufferDestroyer {
      public:
        /** Creates the object that handles thread exit. */
        explicit ThreadBufferDestroyer() {}

        /** Forces creation of the thread-exit object after a queue is allocated. */
        void threadBufferCreated() {}

        /** Marks the queue for deletion after the poller drains its records. */
        ~ThreadBufferDestroyer() {
            if (fmtlog::threadBuffer != nullptr) {
                fmtlog::threadBuffer->shouldDeallocate = true;
                fmtlog::threadBuffer = nullptr;
            }
        }
    };

    /** Registered format metadata for one log site. */
    struct StaticLogInfo {
        
        /** Captures formatter, source location, level, and format string for a log site. */
        constexpr StaticLogInfo(fmtlog::FormatToFn fn,
                                const char* loc,
                                fmtlog::LogLevel level,
                                fmt::string_view fmtString)
            : formatToFn(fn), formatString(fmtString), location(loc), logLevel(level), argIdx(-1) {}

        /** Truncates @c location and records basename offsets for header fields. */
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

        /** Returns the source location after its last path separator, including any line or column. */
        inline fmt::string_view getBase() {
            return fmt::string_view(location + basePos, endPos - basePos);
        }

        /** Returns the truncated full source location slice. */
        inline fmt::string_view getLocation() {
            return fmt::string_view(location, endPos);
        }

        /** Deferred body formatter for this log site, or null for one-shot messages. */
        fmtlog::FormatToFn formatToFn;
        /** Registered format string with named fields converted to argument indices. */
        fmt::string_view formatString;
        /** Source location string, possibly truncated to 255 bytes. */
        const char* location;
        /** Byte offset of the basename within @c location. */
        uint8_t basePos;
        /** Byte length of the active @c location slice. */
        uint8_t endPos;
        /** Severity recorded when the site was registered. */
        fmtlog::LogLevel logLevel;
        /** Index of this site's arguments in the shared fmt argument vector. */
        int argIdx;
    };

    /** At thread exit, this object asks the poller to free the thread's queue. */
    static thread_local ThreadBufferDestroyer sbc;
    /** Nanoseconds since epoch for today's local midnight. */
    int64_t midnightNs;
    /** fmt pattern for the header written before each message body. */
    fmt::string_view headerPattern;
    /** Whether @c headerPattern was heap-allocated and must be freed. */
    bool shouldDeallocateHeader = false;
    /** Destination @c FILE for formatted log output. */
    FILE* outputFp = nullptr;
    /** Whether the logger owns the handle and tracks its file position. */
    bool manageFp = false;
    size_t fpos = 0; // file position of membuf, used only when manageFp == true
    /** Delay before a later poll flushes pending output, in nanoseconds. */
    int64_t flushDelay;
    /** Next scheduled flush time in nanoseconds, or max when idle. */
    int64_t nextFlushTime = (std::numeric_limits<int64_t>::max)();
    /** Internal buffer size threshold that triggers flush. */
    uint32_t flushBufSize = 8 * 1024;
    /** Minimum record level that forces an immediate flush. */
    fmtlog::LogLevel flushLogLevel = fmtlog::OFF;
    /** Protects @c threadBuffers while queues are registered. */
    std::mutex bufferMutex;
    /** Per-thread queues registered since the last poll. */
    std::vector<fmtlog::ThreadBuffer*> threadBuffers;
    /** One thread queue and its next record, used to select the oldest timestamp. */
    struct HeapNode {
        /** Wraps @p buffer for heap ordering during poll. */
        HeapNode(fmtlog::ThreadBuffer* buffer) : tb(buffer) {}

        /** Thread queue supplying records to the poller. */
        fmtlog::ThreadBuffer* tb;
        /** Next pending message from @c tb, or null when the queue is empty. */
        const fmtlog::SPSCVarQueueOPT::MsgHeader* header = nullptr;
    };
    /** Active thread queues merged for timestamp-ordered polling. */
    std::vector<HeapNode> bgThreadBuffers;
    /** Protects @c logInfos while log sites are registered. */
    std::mutex logInfoMutex;
    /** Log-site registrations waiting to merge into @c bgLogInfos. */
    std::vector<StaticLogInfo> logInfos;
    /** Merged log-site table used while draining queues. */
    std::vector<StaticLogInfo> bgLogInfos;

    /** Optional callback for records that pass minCBLogLevel. */
    fmtlog::LogCBFn logCB = nullptr;
    /** Minimum level delivered to @c logCB. */
    fmtlog::LogLevel minCBLogLevel;
    /** Callback invoked when a thread queue cannot accept a record. */
    fmtlog::LogQFullCBFn logQFullCB = fmtlogEmptyFun;
    /** Opaque user data passed to @c logQFullCB. */
    void* logQFullCBArg = nullptr;

    /** Formatted records waiting to be written to the output file. */
    fmtlog::MemoryBuffer membuf;

    /** Count of named placeholders supported by the header pattern. */
    const static int parttenArgSize = 25;
    /** Maps header placeholder order to indices in @c args. */
    uint32_t reorderIdx[parttenArgSize];
    /** Three-letter weekday field for the header pattern. */
    Str<3> weekdayName;
    /** Three-letter month field for the header pattern. */
    Str<3> monthName;
    /** Four-digit year field for the header pattern. */
    Str<4> year;
    char dash1 = '-';
    /** Two-digit month field for the header pattern. */
    Str<2> month;
    char dash2 = '-';
    /** Two-digit day field for the header pattern. */
    Str<2> day;
    char space = ' ';
    /** Two-digit hour field for the header pattern. */
    Str<2> hour;
    char colon1 = ':';
    /** Two-digit minute field for the header pattern. */
    Str<2> minute;
    char colon2 = ':';
    /** Two-digit second field for the header pattern. */
    Str<2> second;
    char dot1 = '.';
    /** Nine-digit fractional second field for the header pattern. */
    Str<9> nanosecond;
    /** Three-letter level field for the header pattern. */
    Str<3> logLevel;
    /** Shared fmt arguments for headers and registered message formats. */
    std::vector<fmt::basic_format_arg<fmtlog::Context>> args;

    /** Requests that the background polling loop keep running. */
    volatile bool threadRunning = false;
    /** Background thread that calls @ref poll on an interval. */
    std::thread thr;

    /** Recomputes local midnight and refreshes calendar header fields. */
    void resetDate() {
        time_t rawtime = fmtlog_wrapper<>::instance.tscns.rdns() / 1000000000;
        struct tm* timeinfo = localtime(&rawtime);
        timeinfo->tm_sec = timeinfo->tm_min = timeinfo->tm_hour = 0;
        midnightNs = mktime(timeinfo) * 1000000000;
        year.fromi(1900 + timeinfo->tm_year);
        month.fromi(1 + timeinfo->tm_mon);
        day.fromi(timeinfo->tm_mday);
        const char* weekdays[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
        weekdayName = weekdays[timeinfo->tm_wday];
        const char* monthNames[12] = {
            "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        monthName = monthNames[timeinfo->tm_mon];
    }

    /** Creates the thread-local queue and registers it for polling. */
    void preallocate() {
        if (fmtlog::threadBuffer)
            return;
        fmtlog::threadBuffer = new fmtlog::ThreadBuffer();
#ifdef _WIN32
        uint32_t tid = static_cast<uint32_t>(::GetCurrentThreadId());
#else
// macOS 10.12 and later provides pthread_threadid_np.
#if defined(__APPLE__) && __MAC_OS_X_VERSION_MIN_REQUIRED >= MAC_OS_X_VERSION_10_12
        uint64_t tid64;
        pthread_threadid_np(NULL, &tid64);
        uint32_t tid = static_cast<uint32_t>(tid64);
#else
        uint32_t tid = static_cast<uint32_t>(::syscall(SYS_gettid));
#endif
#endif
        fmtlog::threadBuffer->nameSize =
            fmt::format_to_n(
                fmtlog::threadBuffer->name, sizeof(fmtlog::threadBuffer->name), "{}", tid)
                .size;
        sbc.threadBufferCreated();

        std::unique_lock<std::mutex> guard(bufferMutex);
        threadBuffers.push_back(fmtlog::threadBuffer);
    }

    /** Stores header format argument @p arg at placeholder index @p I. */
    template <size_t I, typename T> inline void setArg(const T& arg) {
        args[reorderIdx[I]] = arg;
    }

    /** Updates a header argument's value while keeping its cached fmt type. */
    template <size_t I, typename T> inline void setArgVal(const T& arg) {
        fmt::detail::value<fmtlog::Context>& value_ =
            *(fmt::detail::value<fmtlog::Context>*)&args[reorderIdx[I]];
        value_ = arg;
    }

    /** Writes @c membuf to @c outputFp and clears the internal buffer. */
    void flushLogFile() {
        if (outputFp) {
            fwrite(membuf.data(), 1, membuf.size(), outputFp);
            if (!manageFp)
                fflush(outputFp);
            else
                fpos += membuf.size();
        }
        membuf.clear();
        nextFlushTime = (std::numeric_limits<int64_t>::max)();
    }

    /** Flushes pending output and closes a managed log file. */
    void closeLogFile() {
        if (membuf.size())
            flushLogFile();
        if (manageFp)
            fclose(outputFp);
        outputFp = nullptr;
        manageFp = false;
    }

    /** Starts polling at the requested interval, allowing for time spent in poll(). */
    void startPollingThread(int64_t pollInterval) {
        stopPollingThread();
        threadRunning = true;
        thr = std::thread([pollInterval, this]() {
            while (threadRunning) {
                int64_t before = fmtlog_wrapper<>::instance.tscns.rdns();
                poll(false);
                int64_t delay = fmtlog_wrapper<>::instance.tscns.rdns() - before;
                if (delay < pollInterval) {
                    std::this_thread::sleep_for(std::chrono::nanoseconds(pollInterval - delay));
                }
            }
            poll(true);
        });
    }

    /** Stops the background polling thread and joins it. */
    void stopPollingThread() {
        if (!threadRunning)
            return;
        threadRunning = false;
        if (thr.joinable())
            thr.join();
    }

    /** Formats one queued record into @c membuf and may flush or invoke @c logCB. */
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
        int64_t ts = fmtlog_wrapper<>::instance.tscns.tsc2ns(tsc);
        // A record from another thread can predate the current midnight.
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
        fmtlog::vformat_to(
            membuf, headerPattern, fmt::basic_format_args(args.data(), parttenArgSize));
        size_t bodyPos = membuf.size();

        if (info.formatToFn) {
            info.formatToFn(info.formatString, data, membuf, info.argIdx, args);
        } else { // log once
            membuf.append(fmt::string_view(data, end - data));
        }

        if (logCB && info.logLevel >= minCBLogLevel) {
            logCB(ts,
                  info.logLevel,
                  info.getLocation(),
                  info.basePos,
                  threadName,
                  fmt::string_view(membuf.data() + headerPos, membuf.size() - headerPos),
                  bodyPos - headerPos,
                  fpos + headerPos);
        }
        membuf.push_back('\n');
        if (membuf.size() >= flushBufSize || info.logLevel >= flushLogLevel) {
            flushLogFile();
        }
    }

    /** Restores min-heap order for @c bgThreadBuffers starting at index @p i. */
    void adjustHeap(size_t i) {
        while (true) {
            size_t min_i = i;
            for (size_t ch = i * 2 + 1, end = std::min(ch + 2, bgThreadBuffers.size()); ch < end;
                 ch++) {
                auto h_ch = bgThreadBuffers[ch].header;
                auto h_min = bgThreadBuffers[min_i].header;
                if (h_ch && (!h_min || *(int64_t*)(h_ch + 1) < *(int64_t*)(h_min + 1)))
                    min_i = ch;
            }
            if (min_i == i)
                break;
            std::swap(bgThreadBuffers[i], bgThreadBuffers[min_i]);
            i = min_i;
        }
    }

    /**
     * Polls thread queues and writes records in timestamp order.
     *
     * Only consumes records older than this poll's start time whose log-site
     * metadata has arrived. Queue fronts form a min-heap so the next record
     * comes from the thread with the oldest pending timestamp.
     */
    void poll(bool forceFlush) {
        fmtlog_wrapper<>::instance.tscns.calibrate();
        int64_t tsc = fmtlog_wrapper<>::instance.tscns.rdtsc();
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
            if (node.header)
                continue;
            node.header = node.tb->varq.front();
            if (!node.header && node.tb->shouldDeallocate) {
                delete node.tb;
                node = bgThreadBuffers.back();
                bgThreadBuffers.pop_back();
                i--;
            }
        }

        if (bgThreadBuffers.empty())
            return;

        // Order queue fronts so the oldest timestamp is first.
        for (int i = bgThreadBuffers.size() / 2; i >= 0; i--) {
            adjustHeap(i);
        }

        while (true) {
            auto h = bgThreadBuffers[0].header;
            if (!h || h->logId >= bgLogInfos.size() || *(int64_t*)(h + 1) >= tsc)
                break;
            auto tb = bgThreadBuffers[0].tb;
            handleLog(fmt::string_view(tb->name, tb->nameSize), h);
            tb->varq.pop();
            bgThreadBuffers[0].header = tb->varq.front();
            adjustHeap(0);
        }

        if (membuf.size() == 0)
            return;
        if (!manageFp || forceFlush) {
            flushLogFile();
            return;
        }
        int64_t now = fmtlog_wrapper<>::instance.tscns.tsc2ns(tsc);
        if (now > nextFlushTime) {
            flushLogFile();
        } else if (nextFlushTime == (std::numeric_limits<int64_t>::max)()) {
            nextFlushTime = now + flushDelay;
        }
    }
};

template <int _>
thread_local typename fmtlogDetailT<_>::ThreadBufferDestroyer fmtlogDetailT<_>::sbc;

/** Holds the single shared @ref fmtlogDetailT instance for the default tag.
 *
 * @tparam __ Unused wrapper tag matching @ref fmtlogDetailT.
 */
template <int __ = 0> struct fmtlogDetailWrapper {
    /** Process-wide runtime state used by @ref fmtlogT. */
    static fmtlogDetailT<> impl;
};

template <int _> fmtlogDetailT<> fmtlogDetailWrapper<_>::impl;

/** Assigns a stable log id and queues static format metadata for @p logId. */
template <int _>
void fmtlogT<_>::registerLogInfo(uint32_t& logId,
                                 FormatToFn fn,
                                 const char* location,
                                 LogLevel level,
                                 fmt::string_view fmtString) noexcept {
    auto& d = fmtlogDetailWrapper<>::impl;
    std::lock_guard<std::mutex> lock(d.logInfoMutex);
    if (logId)
        return;
    logId = d.logInfos.size() + d.bgLogInfos.size();
    d.logInfos.emplace_back(fn, location, level, fmtString);
}

/** Appends formatted arguments to the supplied memory buffer. */
template <int _>
void fmtlogT<_>::vformat_to(fmtlog::MemoryBuffer& out,
                            fmt::string_view fmt,
                            fmt::format_args args) {
    fmt::detail::vformat_to(out, fmt, args);
}

/** Returns the formatted length of @p fmt without writing output. */
template <int _> size_t fmtlogT<_>::formatted_size(fmt::string_view fmt, fmt::format_args args) {
    auto buf = fmt::detail::counting_buffer<>();
    fmt::detail::vformat_to(buf, fmt, args);
    return buf.count();
}

/** Formats @p fmt with @p args into caller-owned @p out. */
template <int _>
void fmtlogT<_>::vformat_to(char* out, fmt::string_view fmt, fmt::format_args args) {
    fmt::vformat_to(out, fmt, args);
}

/** Reserves @p size payload bytes in the thread queue and may call the queue-full callback. */
template <int _>
typename fmtlogT<_>::SPSCVarQueueOPT::MsgHeader* fmtlogT<_>::allocMsg(uint32_t size,
                                                                      bool q_full_cb) noexcept {
    auto& d = fmtlogDetailWrapper<>::impl;
    if (threadBuffer == nullptr)
        preallocate();
    auto ret = threadBuffer->varq.alloc(size);
    if ((ret == nullptr) & q_full_cb)
        d.logQFullCB(d.logQFullCBArg);
    return ret;
}

/** Forwards queue allocation to @ref SPSCVarQueueOPT::alloc. */
template <int _>
typename fmtlogT<_>::SPSCVarQueueOPT::MsgHeader*
fmtlogT<_>::SPSCVarQueueOPT::allocMsg(uint32_t size) noexcept {
    return alloc(size);
}

template <int _> void fmtlogT<_>::preallocate() noexcept {
    fmtlogDetailWrapper<>::impl.preallocate();
}

template <int _> void fmtlogT<_>::setLogFile(const char* filename, bool truncate) {
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

template <int _> void fmtlogT<_>::setLogFile(FILE* fp, bool manageFp) {
    auto& d = fmtlogDetailWrapper<>::impl;
    closeLogFile();
    if (manageFp) {
        setbuf(fp, nullptr);
        d.fpos = ftell(fp);
    } else
        d.fpos = 0;
    d.outputFp = fp;
    d.manageFp = manageFp;
}

template <int _> void fmtlogT<_>::setFlushDelay(int64_t ns) noexcept {
    fmtlogDetailWrapper<>::impl.flushDelay = ns;
}

template <int _> void fmtlogT<_>::flushOn(LogLevel flushLogLevel) noexcept {
    fmtlogDetailWrapper<>::impl.flushLogLevel = flushLogLevel;
}

template <int _> void fmtlogT<_>::setFlushBufSize(uint32_t bytes) noexcept {
    fmtlogDetailWrapper<>::impl.flushBufSize = bytes;
}

template <int _> void fmtlogT<_>::closeLogFile() noexcept {
    fmtlogDetailWrapper<>::impl.closeLogFile();
}

template <int _> void fmtlogT<_>::poll(bool forceFlush) {
    fmtlogDetailWrapper<>::impl.poll(forceFlush);
}

template <int _> void fmtlogT<_>::setThreadName(const char* name) noexcept {
    preallocate();
    threadBuffer->nameSize =
        fmt::format_to_n(threadBuffer->name, sizeof(fmtlog::threadBuffer->name), "{}", name).size;
}

template <int _> void fmtlogT<_>::setLogCB(LogCBFn cb, LogLevel minCBLogLevel_) noexcept {
    auto& d = fmtlogDetailWrapper<>::impl;
    d.logCB = cb;
    d.minCBLogLevel = minCBLogLevel_;
}

template <int _> void fmtlogT<_>::setLogQFullCB(LogQFullCBFn cb, void* userData) noexcept {
    auto& d = fmtlogDetailWrapper<>::impl;
    d.logQFullCB = cb;
    d.logQFullCBArg = userData;
}

template <int _> void fmtlogT<_>::setHeaderPattern(const char* pattern) {
    fmtlogDetailWrapper<>::impl.setHeaderPattern(pattern);
}

template <int _> void fmtlogT<_>::startPollingThread(int64_t pollInterval) noexcept {
    fmtlogDetailWrapper<>::impl.startPollingThread(pollInterval);
}

template <int _> void fmtlogT<_>::stopPollingThread() noexcept {
    fmtlogDetailWrapper<>::impl.stopPollingThread();
}

/** Explicit instantiation for the default logger tag. */
template class fmtlogT<0>;
