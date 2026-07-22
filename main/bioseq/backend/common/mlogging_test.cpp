#include "common_tests.h"
#include "common.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

import bioseq.common.mlogging;

struct alignas(64) MloggingTrackedArgument {
    int value = 0;

    inline static int live_count = 0;
    inline static int copy_count = 0;
    inline static int move_count = 0;
    inline static int destructor_count = 0;

    explicit MloggingTrackedArgument(int initial_value)
        : value(initial_value)
    {
        ++live_count;
    }

    MloggingTrackedArgument(const MloggingTrackedArgument& other)
        : value(other.value)
    {
        ++live_count;
        ++copy_count;
    }

    MloggingTrackedArgument(MloggingTrackedArgument&& other) noexcept
        : value(other.value)
    {
        ++live_count;
        ++move_count;
    }

    ~MloggingTrackedArgument()
    {
        --live_count;
        ++destructor_count;
    }

    static void reset_counts()
    {
        live_count = 0;
        copy_count = 0;
        move_count = 0;
        destructor_count = 0;
    }
};

template<>
struct fmt::formatter<MloggingTrackedArgument> {
    constexpr auto parse(fmt::format_parse_context& context)
    {
        return context.begin();
    }

    template<typename FormatContext>
    auto format(const MloggingTrackedArgument& argument,
                FormatContext& context) const
    {
        return fmt::format_to(context.out(), "tracked({})", argument.value);
    }
};

// fmtlog intentionally unwraps smart pointers while decoding an asynchronous
// record.  fmt still validates the producer-side checked format string before
// that decode step, so provide the matching test-only formatting contract.
template<>
struct fmt::formatter<std::shared_ptr<int>> : fmt::formatter<int> {
    template<typename FormatContext>
    auto format(const std::shared_ptr<int>& value,
                FormatContext& context) const
    {
        return fmt::formatter<int>::format(*value, context);
    }
};

namespace {

struct CapturedSinkLog {
    LoggingLevel level = LoggingLevel::OFF;
    int raw_pg_elevel = 0;
    bool has_raw_pg_elevel = false;
    std::string message;
    std::string file_name;
    std::uint_least32_t line = 0;
    std::uint_least32_t column = 0;
};

struct CapturedSinkReport {
    LoggingLevel level = LoggingLevel::OFF;
    int raw_pg_elevel = 0;
    bool has_raw_pg_elevel = false;
    int sqlerrcode = 0;
    bool has_sqlerrcode = false;
    std::string message;
    std::string detail;
    std::string hint;
    std::string file_name;
    std::uint_least32_t line = 0;
    std::uint_least32_t column = 0;
};

std::vector<CapturedSinkLog> captured_sink_logs;
std::vector<CapturedSinkReport> captured_sink_reports;

[[nodiscard]] bool ends_with(
    std::string_view value,
    std::string_view suffix)
{
    return value.size() >= suffix.size() &&
        value.substr(value.size() - suffix.size()) == suffix;
}

[[nodiscard]] bool contains_location_line(
    std::string_view location,
    std::string_view file_name,
    std::uint_least32_t line)
{
    const std::string expected = fmt::format("{}:{}:", file_name, line);
    return location.find(expected) != std::string_view::npos;
}

void drain_fmtlog()
{
    for (int i = 0; i < 4; ++i) {
        fmtlog::poll(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    fmtlog::poll(true);
}

bool should_log_level(LoggingLevel) {
    return true;
}

bool should_log_pg(int) {
    return true;
}

void capture_level_log(
    LoggingLevel level,
    BioseqSourceLocation location,
    std::string_view message)
{
    captured_sink_logs.push_back(CapturedSinkLog{
        .level = level,
        .raw_pg_elevel = 0,
        .has_raw_pg_elevel = false,
        .message = std::string(message),
        .file_name = std::string(location.file_name),
        .line = location.line,
        .column = location.column,
    });
}

void capture_raw_sink_log(
    int pg_elevel,
    BioseqSourceLocation location,
    std::string_view message)
{
    captured_sink_logs.push_back(CapturedSinkLog{
        .level = LoggingLevel::OFF,
        .raw_pg_elevel = pg_elevel,
        .has_raw_pg_elevel = true,
        .message = std::string(message),
        .file_name = std::string(location.file_name),
        .line = location.line,
        .column = location.column,
    });
}

void capture_report(const BioseqReportView& report)
{
    captured_sink_reports.push_back(CapturedSinkReport{
        .level = report.level,
        .raw_pg_elevel = report.raw_pg_elevel,
        .has_raw_pg_elevel = report.has_raw_pg_elevel,
        .sqlerrcode = report.sqlerrcode,
        .has_sqlerrcode = report.has_sqlerrcode,
        .message = std::string(report.message),
        .detail = std::string(report.detail),
        .hint = std::string(report.hint),
        .file_name = std::string(report.location.file_name),
        .line = report.location.line,
        .column = report.location.column,
    });
}

struct SinkGuard {
    SinkGuard()
    {
        captured_sink_logs.clear();
        captured_sink_reports.clear();
        bioseq_register_postgres_logging_sink(BioseqPostgresLoggingSink{
            .should_log_level = should_log_level,
            .log_level = capture_level_log,
            .should_log_pg = should_log_pg,
            .log_pg = capture_raw_sink_log,
            .report = capture_report,
        });
    }

    ~SinkGuard()
    {
        bioseq_clear_postgres_logging_sink();
    }
};

struct FallbackFileGuard {
    FILE* file = nullptr;
    bool saved_enable_logging = enable_logging;
    fmtlog::LogLevel saved_log_level = fmtlog::getLogLevel();

    FallbackFileGuard()
    {
        bioseq_clear_postgres_logging_sink();
        drain_fmtlog();
        file = std::tmpfile();
        if (file == nullptr) {
            return;
        }
        enable_logging = true;
        fmtlog::setLogLevel(fmtlog::DBG);
        fmtlog::flushOn(fmtlog::DBG);
        fmtlog::setLogFile(file, false);
    }

    ~FallbackFileGuard()
    {
        drain_fmtlog();
        fmtlog::setLogFile(stderr, false);
        fmtlog::flushOn(fmtlog::OFF);
        fmtlog::setLogLevel(saved_log_level);
        enable_logging = saved_enable_logging;
        if (file != nullptr) {
            std::fclose(file);
        }
    }

    [[nodiscard]] std::string contents() const
    {
        if (file == nullptr) {
            return {};
        }

        drain_fmtlog();
        std::fflush(file);
        std::fseek(file, 0, SEEK_SET);

        std::string out;
        char buffer[4096];
        while (const std::size_t read = std::fread(buffer, 1, sizeof(buffer), file)) {
            out.append(buffer, read);
        }
        std::fseek(file, 0, SEEK_END);
        return out;
    }
};

}  // namespace

TEST_CASE("DOC0297 mlogging PG sink preserves caller locations and report fields",
          "[mlogging][doc0297]") {
    SinkGuard guard;

    const auto log_line = static_cast<std::uint_least32_t>(__LINE__ + 1);
    bioseq_log<INF, PG>("sink location {}", 42);

    const auto raw_log_line = static_cast<std::uint_least32_t>(__LINE__ + 1);
    bioseq_log_pg(17, "raw pg {}", "message");

    const auto report_line = static_cast<std::uint_least32_t>(__LINE__ + 1);
    bioseq_report<WRN, PG>().code(12345).msg("primary {}", 7).detail("detail {}", "x").hint("hint {}", "y");

    REQUIRE(captured_sink_logs.size() == 2);
    CHECK(captured_sink_logs[0].level == LoggingLevel::INF);
    CHECK(!captured_sink_logs[0].has_raw_pg_elevel);
    CHECK(captured_sink_logs[0].message == "sink location 42");
    CHECK(ends_with(captured_sink_logs[0].file_name, "mlogging_test.cpp"));
    CHECK(captured_sink_logs[0].line == log_line);
    CHECK(captured_sink_logs[0].column > 0);

    CHECK(captured_sink_logs[1].has_raw_pg_elevel);
    CHECK(captured_sink_logs[1].raw_pg_elevel == 17);
    CHECK(captured_sink_logs[1].message == "raw pg message");
    CHECK(ends_with(captured_sink_logs[1].file_name, "mlogging_test.cpp"));
    CHECK(captured_sink_logs[1].line == raw_log_line);
    CHECK(captured_sink_logs[1].column > 0);

    REQUIRE(captured_sink_reports.size() == 1);
    const CapturedSinkReport& report = captured_sink_reports.front();
    CHECK(report.level == LoggingLevel::WRN);
    CHECK(!report.has_raw_pg_elevel);
    CHECK(report.has_sqlerrcode);
    CHECK(report.sqlerrcode == 12345);
    CHECK(report.message == "primary 7");
    CHECK(report.detail == "detail x");
    CHECK(report.hint == "hint y");
    CHECK(ends_with(report.file_name, "mlogging_test.cpp"));
    CHECK(report.line == report_line);
    CHECK(report.column > 0);
}

TEST_CASE("DOC0297 mlogging fallback keeps caller location and report component parity",
          "[mlogging][doc0297]") {
    FallbackFileGuard guard;
    REQUIRE(guard.file != nullptr);

    const auto file_log_line = static_cast<std::uint_least32_t>(__LINE__ + 1);
    bioseq_log<INF>("file location {}", 11);
    std::string log_text = guard.contents();
    CHECK(log_text.find("file location 11") != std::string::npos);
    CHECK(contains_location_line(
        log_text,
        "mlogging_test.cpp",
        file_log_line));

    const auto progress_log_line = static_cast<std::uint_least32_t>(__LINE__ + 1);
    bioseq_log_progress<DBG>("progress {}", 3);
    log_text = guard.contents();
    CHECK(log_text.find("progress 3") != std::string::npos);
    CHECK(contains_location_line(
        log_text,
        "mlogging_test.cpp",
        progress_log_line));

    const auto fallback_report_line = static_cast<std::uint_least32_t>(__LINE__ + 1);
    bioseq_report<WRN, PG>().msg("file primary {}", 7).detail("file detail {}", "x").hint("file hint {}", "y");
    log_text = guard.contents();
    CHECK(log_text.find(
        "file primary 7 | DETAIL: file detail x | HINT: file hint y") != std::string::npos);
    CHECK(contains_location_line(
        log_text,
        "mlogging_test.cpp",
        fallback_report_line));
}

TEST_CASE("mlogging byte-packed arguments decode odd-offset scalars safely",
          "[mlogging][ubsan][alignment]") {
    FallbackFileGuard guard;
    REQUIRE(guard.file != nullptr);

    // The terminating NUL makes this first encoded field five bytes long, so
    // both the u32 immediately after it and the u64 after the bool start at
    // deliberately unaligned queue offsets.
    const char odd_prefix[] = "odd!";
    const auto emit_packed = [&](std::uint32_t scalar32,
                                 bool flag,
                                 std::uint64_t scalar64) {
        bioseq_log<INF>("packed {} {:08x} {} {:016x}",
                        odd_prefix, scalar32, flag, scalar64);
    };
    emit_packed(0x01020304u, true, 0x0102030405060708ull);
    emit_packed(0xaabbccddu, false, 0x1112131415161718ull);

    const std::string log_text = guard.contents();
    CHECK(log_text.find(
        "packed odd! 01020304 true 0102030405060708") != std::string::npos);
    CHECK(log_text.find(
        "packed odd! aabbccdd false 1112131415161718") != std::string::npos);
}

TEST_CASE("mlogging aligns retained custom arguments after packed strings",
          "[mlogging][ubsan][alignment]") {
    FallbackFileGuard guard;
    REQUIRE(guard.file != nullptr);

    MloggingTrackedArgument::reset_counts();
    {
        const char odd_prefix[] = "odd!";
        const MloggingTrackedArgument first{17};
        const MloggingTrackedArgument second{29};
        const auto emit_custom = [&](const MloggingTrackedArgument& argument) {
            bioseq_log<INF>("custom {} {}", odd_prefix, argument);
        };

        emit_custom(first);
        emit_custom(second);
        CHECK(MloggingTrackedArgument::copy_count == 2);
        CHECK(MloggingTrackedArgument::move_count == 0);
        CHECK(MloggingTrackedArgument::live_count == 4);

        const std::string log_text = guard.contents();
        CHECK(log_text.find("custom odd! tracked(17)") != std::string::npos);
        CHECK(log_text.find("custom odd! tracked(29)") != std::string::npos);
        CHECK(MloggingTrackedArgument::destructor_count == 2);
        CHECK(MloggingTrackedArgument::live_count == 2);
    }

    CHECK(MloggingTrackedArgument::destructor_count == 4);
    CHECK(MloggingTrackedArgument::live_count == 0);
}

TEST_CASE("mlogging checked argument sizing enforces exact queue record bounds",
          "[mlogging][ubsan][alignment][bounds]") {
    using Queue = fmtlog::SPSCVarQueueOPT;

    auto boundary_queue = std::make_unique<Queue>();
    CHECK(boundary_queue->alloc(Queue::MAX_PAYLOAD_SIZE) != nullptr);

    auto rejected_queue = std::make_unique<Queue>();
    CHECK(rejected_queue->alloc(Queue::FIRST_UNSUPPORTED_PAYLOAD_SIZE) == nullptr);
    CHECK(rejected_queue->alloc(std::numeric_limits<std::uint32_t>::max()) == nullptr);

    MloggingTrackedArgument::reset_counts();
    {
        const MloggingTrackedArgument tracked{41};
        constexpr std::size_t tracked_storage_bound =
            alignof(MloggingTrackedArgument) - 1 + sizeof(MloggingTrackedArgument);
        static_assert(fmtlog::MAX_ARGUMENT_STORAGE_SIZE > tracked_storage_bound + 1);

        // The string's terminating NUL plus the custom object's maximum
        // padding and storage consume the argument budget exactly.
        std::string prefix(
            fmtlog::MAX_ARGUMENT_STORAGE_SIZE - tracked_storage_bound - 1,
            'x');
        std::size_t cstring_sizes[1]{};
        std::size_t remaining = fmtlog::MAX_ARGUMENT_STORAGE_SIZE;
        CHECK(fmtlog::getArgSizes<0>(
            cstring_sizes,
            remaining,
            std::string_view(prefix),
            tracked));
        CHECK(remaining == 0);

        prefix.push_back('x');
        remaining = fmtlog::MAX_ARGUMENT_STORAGE_SIZE;
        CHECK_FALSE(fmtlog::getArgSizes<0>(
            cstring_sizes,
            remaining,
            std::string_view(prefix),
            tracked));

        // Exercise the noexcept public enqueue path as well: sizing rejects
        // this record before the tracked argument is copied or constructed in
        // queue storage.
        std::uint32_t oversized_log_id = 0;
        fmtlogWrapper<>::impl.log(
            oversized_log_id,
            fmtlogWrapper<>::impl.tscns.rdtsc(),
            "mlogging_test.cpp:oversized",
            fmtlog::INF,
            "oversized {} {}",
            std::string_view(prefix),
            tracked);
        CHECK(MloggingTrackedArgument::copy_count == 0);
        CHECK(MloggingTrackedArgument::move_count == 0);
        CHECK(MloggingTrackedArgument::live_count == 1);
    }
    CHECK(MloggingTrackedArgument::destructor_count == 1);
    CHECK(MloggingTrackedArgument::live_count == 0);
}

TEST_CASE("mlogging retains smart pointers and raw trivial arrays until polling",
          "[mlogging][ubsan][alignment][lifetime]") {
    FallbackFileGuard guard;
    REQUIRE(guard.file != nullptr);

    auto retained = std::make_shared<int>(73);
    const int raw_values[] = {4, 5, 6};
    bioseq_log<INF>("indirect {} {}", retained, raw_values);
    CHECK(retained.use_count() == 2);

    const std::string log_text = guard.contents();
    CHECK(log_text.find("indirect 73 [4, 5, 6]") != std::string::npos);
    CHECK(retained.use_count() == 1);
}
