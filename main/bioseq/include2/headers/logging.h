#pragma once

#include <iostream>

#define FMTLOG_LEVEL_DBG 0
#define FMTLOG_LEVEL_INF 1
#define FMTLOG_LEVEL_WRN 2
#define FMTLOG_LEVEL_ERR 3
#define FMTLOG_LEVEL_OFF 4

// define FMTLOG_ACTIVE_LEVEL to turn off low log level in compile time
#ifndef FMTLOG_ACTIVE_LEVEL
#define FMTLOG_ACTIVE_LEVEL FMTLOG_LEVEL_DBG
#endif

#ifndef FMTLOG_QUEUE_SIZE
#define FMTLOG_QUEUE_SIZE (1 << 20)
#endif


#define __FMTLOG_S1(x) #x
#define __FMTLOG_S2(x) __FMTLOG_S1(x)
#define __FMTLOG_LOCATION __FILE__ ":" __FMTLOG_S2(__LINE__)

#define FMTLOG(level, format, ...)                                                                 \
  do {                                                                                             \
    static uint32_t logId = 0;                                                                     \
    if (!fmtlog::checkLogLevel(level)) break;                                                      \
    fmtlogWrapper<>::impl.log(logId, fmtlogWrapper<>::impl.tscns.rdtsc(), __FMTLOG_LOCATION,       \
                              level, format, ##__VA_ARGS__);                                       \
  } while (0)

#define FMTLOG_LIMIT(min_interval, level, format, ...)                                             \
  do {                                                                                             \
    static uint32_t logId = 0;                                                                     \
    static int64_t limitNs = 0;                                                                    \
    if (!fmtlog::checkLogLevel(level)) break;                                                      \
    int64_t tsc = fmtlogWrapper<>::impl.tscns.rdtsc();                                             \
    int64_t ns = fmtlogWrapper<>::impl.tscns.tsc2ns(tsc);                                          \
    if (ns < limitNs) break;                                                                       \
    limitNs = ns + min_interval;                                                                   \
    fmtlogWrapper<>::impl.log(logId, tsc, __FMTLOG_LOCATION, level, format, ##__VA_ARGS__);        \
  } while (0)

#define FMTLOG_ONCE(level, format, ...)                                                            \
  do {                                                                                             \
    if (!fmtlog::checkLogLevel(level)) break;                                                      \
    fmtlogWrapper<>::impl.logOnce(__FMTLOG_LOCATION, level, format, ##__VA_ARGS__);                \
  } while (0)

#if FMTLOG_ACTIVE_LEVEL <= FMTLOG_LEVEL_DBG
#define logd(format, ...) FMTLOG(fmtlog::DBG, format, ##__VA_ARGS__)
#define logdo(format, ...) FMTLOG_ONCE(fmtlog::DBG, format, ##__VA_ARGS__)
#define logdl(min_interval, format, ...) FMTLOG_LIMIT(min_interval, fmtlog::DBG, format, ##__VA_ARGS__)
#else
#define logd(format, ...) (void)0
#define logdo(format, ...) (void)0
#define logdl(min_interval, format, ...) (void)0
#endif

#if FMTLOG_ACTIVE_LEVEL <= FMTLOG_LEVEL_INF
#define logi(format, ...) FMTLOG(fmtlog::INF, format, ##__VA_ARGS__)
#define logio(format, ...) FMTLOG_ONCE(fmtlog::INF, format, ##__VA_ARGS__)
#define logil(min_interval, format, ...) FMTLOG_LIMIT(min_interval, fmtlog::INF, format, ##__VA_ARGS__)
#else
#define logi(format, ...) (void)0
#define logio(format, ...) (void)0
#define logil(min_interval, format, ...) (void)0
#endif

#if FMTLOG_ACTIVE_LEVEL <= FMTLOG_LEVEL_WRN
#define logw(format, ...) FMTLOG(fmtlog::WRN, format, ##__VA_ARGS__)
#define logwo(format, ...) FMTLOG_ONCE(fmtlog::WRN, format, ##__VA_ARGS__)
#define logwl(min_interval, format, ...) FMTLOG_LIMIT(min_interval, fmtlog::WRN, format, ##__VA_ARGS__)
#else
#define logw(format, ...) (void)0
#define logwo(format, ...) (void)0
#define logwl(min_interval, format, ...) (void)0
#endif

#if FMTLOG_ACTIVE_LEVEL <= FMTLOG_LEVEL_ERR
#define loge(format, ...) FMTLOG(fmtlog::ERR, format, ##__VA_ARGS__)
#define logeo(format, ...) FMTLOG_ONCE(fmtlog::ERR, format, ##__VA_ARGS__)
#define logel(min_interval, format, ...) FMTLOG_LIMIT(min_interval, fmtlog::ERR, format, ##__VA_ARGS__)
#else
#define loge(format, ...) (void)0
#define logeo(format, ...) (void)0
#define logel(min_interval, format, ...) (void)0
#endif

#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)
#define AT __FILE__ ":" TOSTRING(__LINE__)
#define XJustLog(...) if(enable_logging) { std::cout << fmt::format(__VA_ARGS__); std::cout << "\n"; std::cout.flush(); std::cout << ""; std::cout.flush(); }
// Function-boundary log. Records into the SPSC log queue; the fmtlog
// background poll thread drains it. The previous implementation called
// `fmtlog::poll(true)` three times per invocation from the producer
// thread, which serialized every JustLog through a flush + write(stderr)
// — turning each call into a 1-5 us syscall round-trip. The flush is
// not needed for correctness (fmtlog has a flush-delay knob and the
// background poll thread runs as soon as the producer enqueues), so we
// simply enqueue and let the consumer drain.
#define JustLog(...) if(enable_logging) { logd(__VA_ARGS__); }
// Hot-loop log. Use inside per-base / per-anchor / per-iteration loops
// where the logging cost would dominate the algorithm itself. Default:
// compiled out (zero cost regardless of `enable_logging`). Set
// `BIOSEQ_HOT_TRACE_LOGS=1` at compile time to turn back on for
// targeted debugging — log cost is then identical to JustLog.
#ifdef BIOSEQ_HOT_TRACE_LOGS
#define JustLogHot(...) JustLog(__VA_ARGS__)
#else
#define JustLogHot(...) ((void)0)
#endif
#define NJustLog(...) if(enable_logging) { elog(NOTICE, fmt::format(__VA_ARGS__).c_str()); }
#define nodeStr(X) ""