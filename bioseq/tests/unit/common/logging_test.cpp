/**
 * @file
 * @brief Verifies logging sinks, fallback files, argument packing, and lifetimes.
 */

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <tests/support/common.hpp>
#include <thread>
#include <vector>

import bioseq.common.logging;

/** @brief Tracks copies, moves, and destruction of queued custom arguments. */
struct alignas(64) logging_tracked_argument {
    int value = 0;

    inline static int live_count = 0;
    inline static int copy_count = 0;
    inline static int move_count = 0;
    inline static int destructor_count = 0;

    /** @brief Constructs a tracked value and records its live instance. */
    explicit logging_tracked_argument(int initial_value) : value(initial_value) {
        ++live_count;
    }

    /** @brief Copies a tracked value and records the retained queue instance. */
    logging_tracked_argument(const logging_tracked_argument& other) : value(other.value) {
        ++live_count;
        ++copy_count;
    }

    /** @brief Moves a tracked value while preserving its formatted value. */
    logging_tracked_argument(logging_tracked_argument&& other) noexcept : value(other.value) {
        ++live_count;
        ++move_count;
    }

    /** @brief Records destruction of one tracked instance. */
    ~logging_tracked_argument() {
        --live_count;
        ++destructor_count;
    }

    /** @brief Resets all lifetime counters before one isolated assertion group. */
    static void reset_counts() {
        live_count = 0;
        copy_count = 0;
        move_count = 0;
        destructor_count = 0;
    }
};

/** @brief Formats the tracked test argument through its retained integer value. */
template <> struct fmt::formatter<logging_tracked_argument> {
    constexpr auto parse(fmt::format_parse_context& context) {
        return context.begin();
    }

    template <typename p_format_context>
    auto format(const logging_tracked_argument& argument, p_format_context& context) const {
        return fmt::format_to(context.out(), "tracked({})", argument.value);
    }
};

/**
 * @brief Formats retained integer ownership through fmtlog's decoded pointer value.
 *
 * This test-only formatter matches the asynchronous decoder's smart-pointer contract.
 */
template <> struct fmt::formatter<std::shared_ptr<int>> : fmt::formatter<int> {
    template <typename p_format_context>
    auto format(const std::shared_ptr<int>& value, p_format_context& context) const {
        return fmt::formatter<int>::format(*value, context);
    }
};

namespace {

/** @brief Captured fields from one message-sink callback. */
struct captured_sink_log {
    logging_level level = logging_level::disabled;
    int postgres_error_level = 0;
    bool has_postgres_error_level = false;
    std::string message;
    std::string file_name;
    std::uint_least32_t line = 0;
    std::uint_least32_t column = 0;
};

/** @brief Captured fields from one structured-report callback. */
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

/**
 * @brief Checks whether a string ends with a suffix.
 * @param[in] value String to inspect.
 * @param[in] suffix Required trailing text.
 * @return `true` when `suffix` matches the end of `value`.
 */
[[nodiscard]] bool ends_with(std::string_view value, std::string_view suffix) {
    return value.size() >= suffix.size() && value.substr(value.size() - suffix.size()) == suffix;
}

/**
 * @brief Checks for an encoded source file and line in fallback output.
 * @param[in] location Formatted location text.
 * @param[in] file_name Expected source filename.
 * @param[in] line Expected source line.
 * @return `true` when the exact location prefix occurs.
 */
[[nodiscard]] bool contains_location_line(std::string_view location,
                                          std::string_view file_name,
                                          std::uint_least32_t line) {
    const std::string expected = fmt::format("{}:{}:", file_name, line);
    return location.find(expected) != std::string_view::npos;
}

/** @brief Polls fmtlog until queued test records have been consumed. */
void drain_fmtlog() {
    for (int poll_index = 0; poll_index < 4; ++poll_index) {
        fmtlog::poll(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    fmtlog::poll(true);
}

/** @brief Enables every project logging level for the capture fixture. */
bool is_logging_level_enabled(logging_level) {
    return true;
}

/** @brief Enables every PostgreSQL error level for the capture fixture. */
bool is_postgres_error_level_enabled(int) {
    return true;
}

/** @brief Copies one typed project log into the capture vector. */
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

/** @brief Copies one raw PostgreSQL log into the capture vector. */
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

/** @brief Copies one structured report into the capture vector. */
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

/** @brief Installs the capture sink for one lexical test scope. */
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
 * @brief Redirects fallback logging to a temporary file for one lexical scope.
 *
 * Destruction drains the queue before restoring the sink and closing the file.
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

    /**
     * @brief Reads all fallback bytes emitted so far and restores append position.
     * @return Complete temporary-file contents.
     */
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

    // The terminating NUL makes this first encoded field five bytes long, so
    // both the u32 immediately after it and the u64 after the bool start at
    // deliberately unaligned queue offsets.
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

        // The string's terminating NUL plus the custom object's maximum
        // padding and storage consume the argument budget exactly.
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

        // Exercise the noexcept public enqueue path as well: sizing rejects
        // this record before the tracked argument is copied or constructed in
        // queue storage.
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
