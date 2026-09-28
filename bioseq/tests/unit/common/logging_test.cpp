/**
 * @file
 * @brief Checks log destinations, queued argument lifetimes and formatting.
 */

#include <cstdio>
#include <fmt/format.h>
#include <catch2/catch_test_macros.hpp>

import std;
import bioseq.common.logging;

/** Counts copies, moves and live objects while log arguments wait in the queue. */
struct alignas(64) logging_tracked_argument {
    int value = 0;

    inline static int live_count = 0;
    inline static int copy_count = 0;
    inline static int move_count = 0;
    inline static int destructor_count = 0;

    /** Creates a value and increments the live-object count. */
    explicit logging_tracked_argument(int initial_value) : value(initial_value) {
        ++live_count;
    }

    /** Copies the value and increments the copy and live-object counts. */
    logging_tracked_argument(const logging_tracked_argument& other) : value(other.value) {
        ++live_count;
        ++copy_count;
    }

    /** Moves the value and increments the move and live-object counts. */
    logging_tracked_argument(logging_tracked_argument&& other) noexcept : value(other.value) {
        ++live_count;
        ++move_count;
    }

    /** Records one destruction and decrements the live-object count. */
    ~logging_tracked_argument() {
        --live_count;
        ++destructor_count;
    }

    /** Clears the counters before a test with no live tracked arguments. */
    static void reset_counts() {
        live_count = 0;
        copy_count = 0;
        move_count = 0;
        destructor_count = 0;
    }
};

/** Formats the test argument as tracked(value). */
template <> struct fmt::formatter<logging_tracked_argument> {
    constexpr auto parse(fmt::format_parse_context& context) {
        return context.begin();
    }

    template <typename p_format_context>
    auto format(const logging_tracked_argument& argument, p_format_context& context) const {
        return fmt::format_to(context.out(), "tracked({})", argument.value);
    }
};

/** Formats a shared pointer by printing the integer it owns. */
template <> struct fmt::formatter<std::shared_ptr<int>> : fmt::formatter<int> {
    template <typename p_format_context>
    auto format(const std::shared_ptr<int>& value, p_format_context& context) const {
        return fmt::formatter<int>::format(*value, context);
    }
};

namespace {

/** Copies a log message, level and  source location for later checks. */
struct captured_sink_log {
    logging_level level = logging_level::disabled;
    int postgres_error_level = 0;
    bool has_postgres_error_level = false;
    std::string message;
    std::string file_name;
    std::uint_least32_t line = 0;
    std::uint_least32_t column = 0;
};

/** Keeps a report's message, detail, hint, error codes and source location */
struct captured_sink_report {
    logging_level level = logging_level::disabled;
    int postgres_error_level = 0;
    bool has_postgres_error_level = false;
    int sql_error_code = 0;
    bool has_sql_error_code = false;
    std::string message;
    std::string detail;
    std::string hint;
    std::string file_name;
    std::uint_least32_t line = 0;
    std::uint_least32_t column = 0;
};

std::vector<captured_sink_log> captured_sink_logs;
std::vector<captured_sink_report> captured_sink_reports;

/** Returns whether value ends with suffix. */
[[nodiscard]] bool ends_with(std::string_view value, std::string_view suffix) {
    return value.size() >= suffix.size() && value.substr(value.size() - suffix.size()) == suffix;
}

/** Looks for file_name:line: in formatted log output. */
[[nodiscard]] bool contains_location_line(std::string_view location,
                                          std::string_view file_name,
                                          std::uint_least32_t line) {
    const std::string expected = fmt::format("{}:{}:", file_name, line);
    return location.find(expected) != std::string_view::npos;
}

/** Polls and flushes fmtlog several times to consume queued test messages. */
void drain_fmtlog() {
    for (int poll_index = 0; poll_index < 4; ++poll_index) {
        fmtlog::poll(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    fmtlog::poll(true);
}

/** Accepts every project log level for the capture sink. */
bool is_logging_level_enabled(logging_level) {
    return true;
}

/** Accepts every PostgreSQL error level for the capture sink. */
bool is_postgres_error_level_enabled(int) {
    return true;
}

/** Saves a project log mjssage and its source location. */
void capture_level_log(logging_level level,
                       bioseq_source_location location,
                       std::string_view message) {
    captured_sink_logs.push_back(captured_sink_log{
        .level = level,
        .postgres_error_level = 0,
        .has_postgres_error_level = false,
        .message = std::string(message),
        .file_name = std::string(location.file_name),
        .line = location.line,
        .column = location.column,
    });
}

/** Saves a log message with its numeric PostgreSQL error level. */
void capture_raw_sink_log(int postgres_error_level,
                          bioseq_source_location location,
                          std::string_view message) {
    captured_sink_logs.push_back(captured_sink_log{
        .level = logging_level::disabled,
        .postgres_error_level = postgres_error_level,
        .has_postgres_error_level = true,
        .message = std::string(message),
        .file_name = std::string(location.file_name),
        .line = location.line,
        .column = location.column,
    });
}

/** Copies a report's fields so tests can inspect them after the callback. */
void capture_report(const bioseq_report_view& report) {
    captured_sink_reports.push_back(captured_sink_report{
        .level = report.level,
        .postgres_error_level = report.postgres_error_level,
        .has_postgres_error_level = report.has_postgres_error_level,
        .sql_error_code = report.sql_error_code,
        .has_sql_error_code = report.has_sql_error_code,
        .message = std::string(report.message),
        .detail = std::string(report.detail),
        .hint = std::string(report.hint),
        .file_name = std::string(report.location.file_name),
        .line = report.location.line,
        .column = report.location.column,
    });
}

/** Clears captured records, installs the test sink and removes it on scope  exit. */
struct sink_guard {
    sink_guard() {
        captured_sink_logs.clear();
        captured_sink_reports.clear();
        bioseq_register_postgres_logging_sink(postgres_logging_sink{
            .is_logging_level_enabled = is_logging_level_enabled,
            .write_message = capture_level_log,
            .is_postgres_error_level_enabled = is_postgres_error_level_enabled,
            .write_postgres_message = capture_raw_sink_log,
            .write_report = capture_report,
        });
    }

    ~sink_guard() {
        bioseq_clear_postgres_logging_sink();
    }
};

/**
 * Sends fallback logs to a temporary file during the test.
 * On scope exit, flushes the queue, restores logging levels and sends output to stderr.
 */
struct fallback_file_guard {
    FILE* file = nullptr;
    bool saved_debug_logging = is_bioseq_debug_logging_enabled();
    fmtlog::LogLevel saved_log_level = fmtlog::getLogLevel();

    fallback_file_guard() {
        bioseq_clear_postgres_logging_sink();
        drain_fmtlog();
        file = std::tmpfile();
        if (file == nullptr) {
            return;
        }
        bioseq_set_fallback_debug_logging_enabled(true);
        fmtlog::setLogLevel(fmtlog::DBG);
        fmtlog::flushOn(fmtlog::DBG);
        fmtlog::setLogFile(file, false);
    }

    ~fallback_file_guard() {
        drain_fmtlog();
        fmtlog::setLogFile(stderr, false);
        fmtlog::flushOn(fmtlog::OFF);
        fmtlog::setLogLevel(saved_log_level);
        bioseq_set_fallback_debug_logging_enabled(saved_debug_logging);
        if (file != nullptr) {
            std::fclose(file);
        }
    }

    /** Flushes queued logs, reads the temporary file and returns its position to the end. */
    [[nodiscard]] std::string contents() const {
        if (file == nullptr) {
            return {};
        }

        drain_fmtlog();
        std::fflush(file);
        std::fseek(file, 0, SEEK_SET);

        std::string out;
        std::array<char, 4096> buffer;
        while (const std::size_t read = std::fread(buffer.data(), 1, buffer.size(), file)) {
            out.append(buffer.data(), read);
        }
        std::fseek(file, 0, SEEK_END);
        return out;
    }
};

} // namespace

TEST_CASE("logging sink preserves caller locations and report fields",
          "[component:bioseq][layer:unit][feature:configuration]") {
    sink_guard guard;

    const auto log_line = static_cast<std::uint_least32_t>(__LINE__ + 1);
    bioseq_log<logging_level::info, logging_target::postgres>("sink location {}", 42);

    const auto raw_log_line = static_cast<std::uint_least32_t>(__LINE__ + 1);
    bioseq_log_pg(17, "raw pg {}", "message");

    const auto report_line = static_cast<std::uint_least32_t>(__LINE__ + 1);
    bioseq_report<logging_level::warning, logging_target::postgres>()
        .with_sql_error_code(12345)
        .with_message("primary {}", 7)
        .with_detail("detail {}", "x")
        .with_hint("hint {}", "y");

    REQUIRE(captured_sink_logs.size() == 2);
    CHECK(captured_sink_logs[0].level == logging_level::info);
    CHECK(!captured_sink_logs[0].has_postgres_error_level);
    CHECK(captured_sink_logs[0].message == "sink location 42");
    CHECK(ends_with(captured_sink_logs[0].file_name, "logging_test.cpp"));
    CHECK(captured_sink_logs[0].line == log_line);
    CHECK(captured_sink_logs[0].column > 0);

    CHECK(captured_sink_logs[1].has_postgres_error_level);
    CHECK(captured_sink_logs[1].postgres_error_level == 17);
    CHECK(captured_sink_logs[1].message == "raw pg message");
    CHECK(ends_with(captured_sink_logs[1].file_name, "logging_test.cpp"));
    CHECK(captured_sink_logs[1].line == raw_log_line);
    CHECK(captured_sink_logs[1].column > 0);

    REQUIRE(captured_sink_reports.size() == 1);
    const captured_sink_report& report = captured_sink_reports.front();
    CHECK(report.level == logging_level::warning);
    CHECK(!report.has_postgres_error_level);
    CHECK(report.has_sql_error_code);
    CHECK(report.sql_error_code == 12345);
    CHECK(report.message == "primary 7");
    CHECK(report.detail == "detail x");
    CHECK(report.hint == "hint y");
    CHECK(ends_with(report.file_name, "logging_test.cpp"));
    CHECK(report.line == report_line);
    CHECK(report.column > 0);
}

TEST_CASE("logging fallback keeps caller location and report components",
          "[component:bioseq][layer:unit][feature:configuration][resource:filesystem]") {
    fallback_file_guard guard;
    REQUIRE(guard.file != nullptr);

    const auto file_log_line = static_cast<std::uint_least32_t>(__LINE__ + 1);
    bioseq_log<logging_level::info>("file location {}", 11);
    std::string log_text = guard.contents();
    CHECK(log_text.find("file location 11") != std::string::npos);
    CHECK(contains_location_line(log_text, "logging_test.cpp", file_log_line));

    const auto progress_log_line = static_cast<std::uint_least32_t>(__LINE__ + 1);
    bioseq_log_progress<logging_level::debug>("progress {}", 3);
    log_text = guard.contents();
    CHECK(log_text.find("progress 3") != std::string::npos);
    CHECK(contains_location_line(log_text, "logging_test.cpp", progress_log_line));

    const auto fallback_report_line = static_cast<std::uint_least32_t>(__LINE__ + 1);
    bioseq_report<logging_level::warning, logging_target::postgres>()
        .with_message("file primary {}", 7)
        .with_detail("file detail {}", "x")
        .with_hint("file hint {}", "y");
    log_text = guard.contents();
    CHECK(log_text.find("file primary 7 | DETAIL: file detail x | HINT: file hint y") !=
          std::string::npos);
    CHECK(contains_location_line(log_text, "logging_test.cpp", fallback_report_line));
}

TEST_CASE("logging byte-packed arguments decode odd-offset scalars safely",
          "[component:bioseq][layer:unit][feature:configuration][resource:filesystem]") {
    fallback_file_guard guard;
    REQUIRE(guard.file != nullptr);

    // Including its NUL, this string takes five bytes. The following integers
    // therefore test decoding from queue offsets that lack natural alignment.
    const char odd_prefix[] = "odd!";
    const auto emit_packed =
        [&odd_prefix](std::uint32_t scalar32, bool flag, std::uint64_t scalar64) {
            bioseq_log<logging_level::info>(
                "packed {} {:08x} {} {:016x}", odd_prefix, scalar32, flag, scalar64);
        };
    emit_packed(0x01020304u, true, 0x0102030405060708ull);
    emit_packed(0xaabbccddu, false, 0x1112131415161718ull);

    const std::string log_text = guard.contents();
    CHECK(log_text.find("packed odd! 01020304 true 0102030405060708") != std::string::npos);
    CHECK(log_text.find("packed odd! aabbccdd false 1112131415161718") != std::string::npos);
}

TEST_CASE("logging aligns retained custom arguments after packed strings",
          "[component:bioseq][layer:unit][feature:configuration][resource:filesystem]") {
    fallback_file_guard guard;
    REQUIRE(guard.file != nullptr);

    logging_tracked_argument::reset_counts();
    {
        const char odd_prefix[] = "odd!";
        const logging_tracked_argument first{17};
        const logging_tracked_argument second{29};
        const auto emit_custom = [&odd_prefix](const logging_tracked_argument& argument) {
            bioseq_log<logging_level::info>("custom {} {}", odd_prefix, argument);
        };

        emit_custom(first);
        emit_custom(second);
        CHECK(logging_tracked_argument::copy_count == 2);
        CHECK(logging_tracked_argument::move_count == 0);
        CHECK(logging_tracked_argument::live_count == 4);

        const std::string log_text = guard.contents();
        CHECK(log_text.find("custom odd! tracked(17)") != std::string::npos);
        CHECK(log_text.find("custom odd! tracked(29)") != std::string::npos);
        CHECK(logging_tracked_argument::destructor_count == 2);
        CHECK(logging_tracked_argument::live_count == 2);
    }

    CHECK(logging_tracked_argument::destructor_count == 4);
    CHECK(logging_tracked_argument::live_count == 0);
}

TEST_CASE("logging checked argument sizing enforces queue record bounds",
          "[component:bioseq][layer:unit][feature:configuration]") {
    using queue_type = fmtlog::SPSCVarQueueOPT;

    auto boundary_queue = std::make_unique<queue_type>();
    CHECK(boundary_queue->alloc(queue_type::MAX_PAYLOAD_SIZE) != nullptr);

    auto rejected_queue = std::make_unique<queue_type>();
    CHECK(rejected_queue->alloc(queue_type::FIRST_UNSUPPORTED_PAYLOAD_SIZE) == nullptr);
    CHECK(rejected_queue->alloc(std::numeric_limits<std::uint32_t>::max()) == nullptr);

    logging_tracked_argument::reset_counts();
    {
        const logging_tracked_argument tracked{41};
        constexpr std::size_t tracked_storage_bound =
            alignof(logging_tracked_argument) - 1 + sizeof(logging_tracked_argument);
        static_assert(fmtlog::MAX_ARGUMENT_STORAGE_SIZE > tracked_storage_bound + 1);

        // Fill the argument budget with the string, its NUL and the custom object
        // including the maximum alignment padding.
        std::string prefix(fmtlog::MAX_ARGUMENT_STORAGE_SIZE - tracked_storage_bound - 1, 'x');
        std::array<std::size_t, 1> cstring_sizes{};
        std::size_t remaining = fmtlog::MAX_ARGUMENT_STORAGE_SIZE;
        CHECK(fmtlog::getArgSizes<0>(
            cstring_sizes.data(), remaining, std::string_view(prefix), tracked));
        CHECK(remaining == 0);

        prefix.push_back('x');
        remaining = fmtlog::MAX_ARGUMENT_STORAGE_SIZE;
        CHECK_FALSE(fmtlog::getArgSizes<0>(
            cstring_sizes.data(), remaining, std::string_view(prefix), tracked));

        // The public enqueue call must reject the oversized record before copying
        // the  tracked object into the queue.
        std::uint32_t oversized_log_id = 0;
        fmtlog_wrapper<>::instance.log(oversized_log_id,
                                       fmtlog_wrapper<>::instance.tscns.rdtsc(),
                                       "logging_test.cpp:oversized",
                                       fmtlog::INF,
                                       "oversized {} {}",
                                       std::string_view(prefix),
                                       tracked);
        CHECK(logging_tracked_argument::copy_count == 0);
        CHECK(logging_tracked_argument::move_count == 0);
        CHECK(logging_tracked_argument::live_count == 1);
    }
    CHECK(logging_tracked_argument::destructor_count == 1);
    CHECK(logging_tracked_argument::live_count == 0);
}

TEST_CASE("logging retains smart pointers and raw trivial arrays until polling",
          "[component:bioseq][layer:unit][feature:configuration][resource:filesystem]") {
    fallback_file_guard guard;
    REQUIRE(guard.file != nullptr);

    auto retained = std::make_shared<int>(73);
    const int raw_values[] = {4, 5, 6};
    bioseq_log<logging_level::info>("indirect {} {}", retained, raw_values);
    CHECK(retained.use_count() == 2);

    const std::string log_text = guard.contents();
    CHECK(log_text.find("indirect 73 [4, 5, 6]") != std::string::npos);
    CHECK(retained.use_count() == 1);
}
