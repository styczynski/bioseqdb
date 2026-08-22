/**
 * @file
 * @brief Provides Bioseq logging, structured diagnostics, and sink routing.
 */
module;

#include <cstdio>

export module bioseq.common.logging;

export import :fmtlog_backend;
export import :formatters;

import bioseq.common.hashing;
import std;

/** @brief Severity threshold and routing severity for a Bioseq log record. */
export enum class logging_level : std::uint8_t {
    debug = 0,
    info = 1,
    warning = 2,
    error = 3,
    disabled = 4,
};

/** @brief Destination family selected for a Bioseq log record. */
export enum class logging_target : std::uint8_t {
    file = 0,
    postgres = 1,
};

/** @brief Lowest severity compiled into the logging interface. */
inline constexpr logging_level k_minimum_compiled_logging_level = logging_level::debug;

/**
 * @brief Stable, non-owning source coordinates passed across logging callbacks.
 *
 * The character pointers refer to compiler-owned strings with static storage duration.
 * Field order is part of the callback boundary and must remain stable.
 */
export struct bioseq_source_location {
    const char* file_name = "";
    const char* function_name = "";
    std::uint_least32_t line = 0;
    std::uint_least32_t column = 0;
};

/**
 * @brief Converts a standard source location to the callback-safe Bioseq view.
 *
 * @param[in] location Compiler-provided source coordinates.
 * @return Non-owning coordinates whose strings have static storage duration.
 */
export inline bioseq_source_location make_bioseq_source_location(
    std::source_location location) noexcept
{
    return bioseq_source_location{
        .file_name = location.file_name(),
        .function_name = location.function_name(),
        .line = location.line(),
        .column = location.column(),
    };
}

/**
 * @brief Non-owning report payload delivered synchronously to a logging sink.
 *
 * String views remain valid only for the duration of the callback. Field order and value
 * representation are stable parts of the sink boundary.
 */
export struct bioseq_report_view {
    logging_level level = logging_level::error;
    int postgres_error_level = 0;
    bool has_postgres_error_level = false;
    int sql_error_code = 0;
    bool has_sql_error_code = false;
    std::string_view message;
    std::string_view detail;
    std::string_view hint;
    bioseq_source_location location;
};

/**
 * @brief Callback table that connects common logging to a PostgreSQL adapter.
 *
 * The registering adapter owns the pointed-to flag and every callback for as long as the
 * sink remains installed. Callbacks must be thread-safe. The table is copied atomically
 * with respect to registration and clearing. Its field order and pointer representation
 * remain stable at the module boundary.
 */
export struct postgres_logging_sink {
    const bool* debug_logging_enabled = nullptr;

    bool (*is_logging_level_enabled)(logging_level level) = nullptr;

    void (*write_message)(
        logging_level level,
        bioseq_source_location location,
        std::string_view message) = nullptr;

    bool (*is_postgres_error_level_enabled)(int postgres_error_level) = nullptr;

    void (*write_postgres_message)(
        int postgres_error_level,
        bioseq_source_location location,
        std::string_view message) = nullptr;

    void (*write_report)(const bioseq_report_view& report) = nullptr;
};

export namespace bioseq::common::logging_detail {

/**
 * @brief Couples a compile-time format string with its caller source location.
 *
 * @tparam p_argument_types Argument types accepted by the format string.
 */
template<typename... p_argument_types>
struct format_with_location {
    fmt::format_string<p_argument_types...> format_string;
    std::source_location location;

    /**
     * @brief Captures the call site while validating the format string at compile time.
     *
     * @tparam p_source_format Source format wrapper type.
     * @param[in] source_format Compile-time format string.
     * @param[in] source_location Caller coordinates. Defaults to the construction site.
     */
    template<typename p_source_format>
        requires std::convertible_to<
            const p_source_format&,
            fmt::format_string<p_argument_types...>>
    consteval format_with_location(
        const p_source_format& source_format,
        std::source_location source_location = std::source_location::current()) noexcept
        : format_string(source_format), location(source_location)
    {}
};

/**
 * @brief Replaces a buffer with a formatted, null-terminated message.
 *
 * @tparam p_argument_types Format argument types.
 * @param[out] target Destination buffer. It ends in a null byte after success.
 * @param[in] format_string Compile-time checked format string.
 * @param[in] arguments Values interpolated into the message.
 */
template<typename... p_argument_types>
[[gnu::always_inline]] inline void format_into(
    fmt::memory_buffer& target,
    fmt::format_string<p_argument_types...> format_string,
    p_argument_types&&... arguments)
{
    target.clear();
    fmt::format_to(
        std::back_inserter(target),
        format_string,
        std::forward<p_argument_types>(arguments)...);
    target.push_back('\0');
}

} // namespace bioseq::common::logging_detail

namespace bioseq::common::logging_detail {

/**
 * @brief Identity of one file-backed logging call site.
 *
 * File and format views refer to static compiler-owned storage. The complete tuple is the
 * stable source-record key. Its fields and hash inputs must not be weakened or reordered.
 */
struct log_site_key {
    std::string_view file_name;
    std::string_view format_string;
    std::uint_least32_t line;
    std::uint_least32_t column;
    std::uint8_t severity;
};

/** @brief Hashes every field of a stable logging-site identity. */
struct log_site_key_hash {
    /**
     * @brief Computes the FNV-based hash used by the logging-site table.
     *
     * @param[in] key Stable call-site identity.
     * @return Combined hash of file, format, coordinates, and severity.
     */
    std::size_t operator()(const log_site_key& key) const noexcept
    {
        std::uint64_t hash_value = bioseq::common::fnv1a_64(key.file_name);
        hash_value = bioseq::common::hash_combine_64(
            hash_value,
            bioseq::common::fnv1a_64(key.format_string));
        hash_value = bioseq::common::hash_combine_64(hash_value, key.line);
        hash_value = bioseq::common::hash_combine_64(hash_value, key.column);
        hash_value = bioseq::common::hash_combine_64(hash_value, key.severity);
        return static_cast<std::size_t>(hash_value);
    }
};

/** @brief Compares complete logging-site identities. */
struct log_site_key_equal {
    /**
     * @brief Tests every source-record key field for equality.
     *
     * @param[in] first First identity.
     * @param[in] second Second identity.
     * @return True only when both identities describe the same call site.
     */
    bool operator()(
        const log_site_key& first,
        const log_site_key& second) const noexcept
    {
        return first.line == second.line &&
               first.column == second.column &&
               first.severity == second.severity &&
               first.file_name == second.file_name &&
               first.format_string == second.format_string;
    }
};

/**
 * @brief Owns fmtlog registration state for one stable call site.
 *
 * The entry address remains stable because the table owns it indirectly. Publication is
 * lock-free after the first successful registration. Registration itself is serialized.
 */
struct log_site_entry {
    std::atomic<std::uint32_t> published_fmtlog_id{0};
    std::uint32_t registration_fmtlog_id = 0;
    std::mutex registration_mutex;
    std::string formatted_location;
};

/** Coordinates read-mostly access to the process-wide call-site table. */
inline std::shared_mutex log_site_table_mutex;
/** Owns one stable registration entry per complete source-record key. */
inline std::unordered_map<
    log_site_key,
    std::unique_ptr<log_site_entry>,
    log_site_key_hash,
    log_site_key_equal> log_site_table;

/** Serializes PostgreSQL sink replacement and snapshotting. */
inline std::mutex postgres_sink_mutex;
/** Callback table copied under postgres_sink_mutex. */
inline postgres_logging_sink postgres_sink;
/** Published pointer to the PostgreSQL-owned debug flag. */
inline std::atomic<const bool*> postgres_debug_logging_enabled{nullptr};
/** Debug flag used when PostgreSQL has not registered a sink. */
inline std::atomic<bool> fallback_debug_logging_enabled{false};

/**
 * @brief Formats source coordinates for fmtlog's location field.
 *
 * @param[in] location Stable callback-safe source coordinates.
 * @return File, line, and column separated by colons.
 */
[[nodiscard]] inline std::string make_fmtlog_location(bioseq_source_location location)
{
    return fmt::format(
        "{}:{}:{}",
        location.file_name,
        location.line,
        location.column);
}

/**
 * @brief Finds or creates the stable registration entry for a file log site.
 *
 * @param[in] location Static-lifetime source coordinates.
 * @param[in] severity Logging severity that participates in site identity.
 * @param[in] format_string Static-lifetime format text that participates in site identity.
 * @return Stable entry owned by the process-wide site table.
 * @note Concurrent readers share the table. Only first insertion takes the exclusive lock.
 */
inline log_site_entry& get_or_create_file_log_site(
    bioseq_source_location location,
    logging_level severity,
    fmt::string_view format_string)
{
    const log_site_key key{
        .file_name = location.file_name,
        .format_string = std::string_view(format_string.data(), format_string.size()),
        .line = location.line,
        .column = location.column,
        .severity = static_cast<std::uint8_t>(severity),
    };

    {
        std::shared_lock read_lock(log_site_table_mutex);
        if (auto iterator = log_site_table.find(key); iterator != log_site_table.end()) {
            return *iterator->second;
        }
    }

    std::unique_lock write_lock(log_site_table_mutex);
    if (auto iterator = log_site_table.find(key); iterator != log_site_table.end()) {
        return *iterator->second;
    }

    auto new_entry = std::make_unique<log_site_entry>();
    new_entry->formatted_location = make_fmtlog_location(location);
    auto& stored_entry = *new_entry;
    log_site_table.emplace(key, std::move(new_entry));
    return stored_entry;
}

/**
 * @brief Maps Bioseq severity to the embedded fmtlog severity without changing values.
 *
 * @param[in] severity Bioseq severity.
 * @return Corresponding fmtlog severity.
 */
constexpr fmtlog::LogLevel to_fmtlog_level(logging_level severity) noexcept
{
    switch (severity) {
        case logging_level::debug:
            return fmtlog::DBG;
        case logging_level::info:
            return fmtlog::INF;
        case logging_level::warning:
            return fmtlog::WRN;
        case logging_level::error:
            return fmtlog::ERR;
        case logging_level::disabled:
            return fmtlog::OFF;
    }
    return fmtlog::ERR;
}

/**
 * @brief Tests whether a severity is retained by the compile-time threshold.
 *
 * @param[in] severity Severity to test.
 * @return True when records at this severity are compiled in.
 */
constexpr bool is_compiled_level_enabled(logging_level severity) noexcept
{
    return severity != logging_level::disabled &&
           static_cast<std::uint8_t>(severity) >=
               static_cast<std::uint8_t>(k_minimum_compiled_logging_level);
}

/**
 * @brief Selects the file-fallback severity for a raw PostgreSQL error level.
 *
 * @param[in] postgres_error_level Raw PostgreSQL error level. Retained for sink symmetry.
 * @return Debug severity, matching the established fallback policy.
 */
inline logging_level fallback_logging_level_for_postgres_error(
    [[maybe_unused]] int postgres_error_level) noexcept
{
    return logging_level::debug;
}

/**
 * @brief Takes a synchronized snapshot of the registered PostgreSQL sink.
 *
 * @return Callback table safe to inspect after releasing the registration mutex.
 */
inline postgres_logging_sink get_postgres_logging_sink() noexcept
{
    std::lock_guard sink_lock(postgres_sink_mutex);
    return postgres_sink;
}

/**
 * @brief Tests the registered PostgreSQL flag or the process-local fallback flag.
 *
 * @return Current debug-logging state.
 */
[[nodiscard]] inline bool is_debug_logging_enabled() noexcept
{
    const bool* postgres_flag =
        postgres_debug_logging_enabled.load(std::memory_order_acquire);
    return postgres_flag != nullptr
        ? *postgres_flag
        : fallback_debug_logging_enabled.load(std::memory_order_relaxed);
}

/**
 * @brief Applies compile-time and debug-runtime gates shared by all log emitters.
 *
 * @tparam p_level Severity selected at the call site.
 * @return True when the call may continue to its target-specific gate.
 */
template<logging_level p_level>
[[gnu::always_inline]] inline bool is_logging_level_active() noexcept
{
    if constexpr (!is_compiled_level_enabled(p_level)) {
        return false;
    }
    if constexpr (p_level == logging_level::debug) {
        return is_debug_logging_enabled();
    }
    return true;
}

/**
 * @brief Emits with a cached fmtlog identifier or serializes first registration.
 *
 * @tparam p_emitter Callable accepting the mutable fmtlog identifier.
 * @param[in,out] site Stable registration state for one logging call site.
 * @param[in] emitter Callable that registers or emits the record.
 */
template<typename p_emitter>
[[gnu::always_inline]] inline void emit_fmtlog_for_site(
    log_site_entry& site,
    p_emitter&& emitter)
{
    std::uint32_t cached_id =
        site.published_fmtlog_id.load(std::memory_order_acquire);
    if (cached_id != 0) {
        emitter(cached_id);
        return;
    }

    std::lock_guard registration_lock(site.registration_mutex);
    emitter(site.registration_fmtlog_id);
    if (site.registration_fmtlog_id != 0) {
        site.published_fmtlog_id.store(
            site.registration_fmtlog_id,
            std::memory_order_release);
    }
}

/**
 * @brief Emits through fmtlog after resolving the stable source-site registration.
 *
 * @tparam p_level Severity selected at compile time.
 * @tparam p_argument_types Format argument types.
 * @param[in] format_location Checked format string and caller coordinates.
 * @param[in] arguments Values forwarded to fmtlog's asynchronous queue.
 */
template<logging_level p_level, typename... p_argument_types>
[[gnu::always_inline]] inline void write_file_log(
    format_with_location<std::type_identity_t<p_argument_types>...> format_location,
    p_argument_types&&... arguments)
{
    const auto fmtlog_level = to_fmtlog_level(p_level);
    if (!fmtlog::checkLogLevel(fmtlog_level)) {
        return;
    }

    auto& site = get_or_create_file_log_site(
        make_bioseq_source_location(format_location.location),
        p_level,
        fmt::string_view(format_location.format_string));
    const auto timestamp_counter = fmtlog_wrapper<>::instance.tscns.rdtsc();
    emit_fmtlog_for_site(
        site,
        [&site,
         &timestamp_counter,
         &fmtlog_level,
         &format_location,
         &arguments...](std::uint32_t& log_id) {
        fmtlog_wrapper<>::instance.log(
            log_id,
            timestamp_counter,
            site.formatted_location.c_str(),
            fmtlog_level,
            format_location.format_string,
            std::forward<p_argument_types>(arguments)...);
        });
}

/**
 * @brief Emits a preformatted message at one compile-time severity.
 *
 * @tparam p_level Severity selected at compile time.
 * @param[in] location Callback-safe source coordinates.
 * @param[in] message Message bytes. No terminator is required.
 */
template<logging_level p_level>
inline void write_preformatted_file_log_at_level(
    bioseq_source_location location,
    std::string_view message)
{
    const auto fmtlog_level = to_fmtlog_level(p_level);
    if (!fmtlog::checkLogLevel(fmtlog_level)) {
        return;
    }

    auto& site = get_or_create_file_log_site(
        location,
        p_level,
        fmt::string_view("{}"));
    const auto timestamp_counter = fmtlog_wrapper<>::instance.tscns.rdtsc();
    emit_fmtlog_for_site(
        site,
        [&site, &timestamp_counter, &fmtlog_level, &message](
            std::uint32_t& log_id) {
        fmtlog_wrapper<>::instance.log(
            log_id,
            timestamp_counter,
            site.formatted_location.c_str(),
            fmtlog_level,
            "{}",
            message);
        });
}

/**
 * @brief Dispatches a preformatted message to the matching compile-time fmtlog path.
 *
 * @param[in] severity Runtime severity to dispatch.
 * @param[in] location Callback-safe source coordinates.
 * @param[in] message Message bytes. No terminator is required.
 */
inline void write_preformatted_file_log(
    logging_level severity,
    bioseq_source_location location,
    std::string_view message)
{
    switch (severity) {
        case logging_level::error:
            write_preformatted_file_log_at_level<logging_level::error>(location, message);
            break;
        case logging_level::warning:
            write_preformatted_file_log_at_level<logging_level::warning>(location, message);
            break;
        case logging_level::info:
            write_preformatted_file_log_at_level<logging_level::info>(location, message);
            break;
        case logging_level::debug:
            if (is_debug_logging_enabled()) {
                write_preformatted_file_log_at_level<logging_level::debug>(location, message);
            }
            break;
        case logging_level::disabled:
            break;
    }
}

/**
 * @brief Formats the message, detail, and hint fields into one fallback line.
 *
 * @param[out] output Destination buffer, replaced without a trailing null byte.
 * @param[in] report Non-owning report payload.
 */
inline void format_report_line(
    fmt::memory_buffer& output,
    const bioseq_report_view& report)
{
    output.clear();
    if (!report.message.empty()) {
        fmt::format_to(std::back_inserter(output), "{}", report.message);
    }
    if (!report.detail.empty()) {
        fmt::format_to(
            std::back_inserter(output),
            "{}DETAIL: {}",
            output.size() == 0 ? "" : " | ",
            report.detail);
    }
    if (!report.hint.empty()) {
        fmt::format_to(
            std::back_inserter(output),
            "{}HINT: {}",
            output.size() == 0 ? "" : " | ",
            report.hint);
    }
}

/**
 * @brief Sends a structured report through the file fallback sink.
 *
 * @param[in] report Non-owning report payload consumed synchronously.
 */
inline void write_report_to_file(const bioseq_report_view& report)
{
    fmt::memory_buffer formatted_line;
    format_report_line(formatted_line, report);
    write_preformatted_file_log(
        report.level,
        report.location,
        std::string_view(formatted_line.data(), formatted_line.size()));
}

} // namespace bioseq::common::logging_detail

/**
 * @brief Installs the process-wide PostgreSQL logging adapter.
 *
 * @param[in] logging_sink Callback table whose pointees outlive registration.
 * @note Replacing a sink is synchronized with readers taking sink snapshots.
 */
export inline void bioseq_register_postgres_logging_sink(
    postgres_logging_sink logging_sink) noexcept
{
    std::lock_guard sink_lock(bioseq::common::logging_detail::postgres_sink_mutex);
    bioseq::common::logging_detail::postgres_sink = logging_sink;
    bioseq::common::logging_detail::postgres_debug_logging_enabled.store(
        logging_sink.debug_logging_enabled,
        std::memory_order_release);
}

/**
 * @brief Removes the process-wide PostgreSQL logging adapter.
 *
 * Subsequent PostgreSQL-targeted records use the file fallback.
 */
export inline void bioseq_clear_postgres_logging_sink() noexcept
{
    std::lock_guard sink_lock(bioseq::common::logging_detail::postgres_sink_mutex);
    bioseq::common::logging_detail::postgres_sink = {};
    bioseq::common::logging_detail::postgres_debug_logging_enabled.store(
        nullptr,
        std::memory_order_release);
}

/**
 * @brief Tests whether debug logging is enabled by the active flag owner.
 *
 * @return PostgreSQL's flag when a sink is installed, otherwise the fallback flag.
 */
export [[nodiscard]] inline bool is_bioseq_debug_logging_enabled() noexcept
{
    return bioseq::common::logging_detail::is_debug_logging_enabled();
}

/**
 * @brief Sets debug logging used when no PostgreSQL sink owns the flag.
 *
 * @param[in] enabled New fallback state.
 */
export inline void bioseq_set_fallback_debug_logging_enabled(
    bool enabled) noexcept
{
    bioseq::common::logging_detail::fallback_debug_logging_enabled.store(
        enabled,
        std::memory_order_relaxed);
}

/**
 * @brief Tests compile-time, runtime, target, and fmtlog gates for a log call.
 *
 * @tparam p_level Severity selected at compile time.
 * @tparam p_target Destination selected at compile time.
 * @return True when constructing and submitting the record is currently useful.
 */
export template<
    logging_level p_level = logging_level::debug,
    logging_target p_target = logging_target::file>
[[nodiscard]] inline bool is_bioseq_log_enabled() noexcept
{
    if (!bioseq::common::logging_detail::is_logging_level_active<p_level>()) {
        return false;
    }

    if constexpr (p_target == logging_target::postgres) {
        auto logging_sink = bioseq::common::logging_detail::get_postgres_logging_sink();
        if (logging_sink.is_logging_level_enabled != nullptr) {
            return logging_sink.is_logging_level_enabled(p_level);
        }
    }

    return fmtlog::checkLogLevel(
        bioseq::common::logging_detail::to_fmtlog_level(p_level));
}

/**
 * @brief Formats and submits one Bioseq log record with caller coordinates.
 *
 * @tparam p_level Severity selected at compile time.
 * @tparam p_target Destination selected at compile time.
 * @tparam p_argument_types Format argument types.
 * @param[in] format_location Checked format string and caller coordinates.
 * @param[in] arguments Values forwarded to the selected sink.
 *
 * @code{.cpp}
 * bioseq_log<logging_level::info>("indexed {} sequences", sequence_count);
 * @endcode
 */
export template<
    logging_level p_level = logging_level::debug,
    logging_target p_target = logging_target::file,
    typename... p_argument_types>
[[gnu::always_inline]] inline void bioseq_log(
    bioseq::common::logging_detail::format_with_location<
        std::type_identity_t<p_argument_types>...> format_location,
    p_argument_types&&... arguments)
{
    if (!bioseq::common::logging_detail::is_logging_level_active<p_level>()) {
        return;
    }

    if constexpr (p_target == logging_target::postgres) {
        auto logging_sink = bioseq::common::logging_detail::get_postgres_logging_sink();
        if (logging_sink.write_message != nullptr) {
            if (logging_sink.is_logging_level_enabled != nullptr &&
                !logging_sink.is_logging_level_enabled(p_level)) {
                return;
            }

            fmt::memory_buffer message;
            bioseq::common::logging_detail::format_into(
                message,
                format_location.format_string,
                std::forward<p_argument_types>(arguments)...);
            logging_sink.write_message(
                p_level,
                make_bioseq_source_location(format_location.location),
                std::string_view(message.data(), message.size() - 1));
            return;
        }
    }

    bioseq::common::logging_detail::write_file_log<p_level>(
        format_location,
        std::forward<p_argument_types>(arguments)...);
}

export {

/**
 * @brief Emits a route rejection summary through the specialized file-log path.
 *
 * @param[in] format_location Checked format string and caller coordinates.
 * @param[in] route Route identifier referenced for the duration of the call.
 * @param[in] total_rejections Rejection count.
 */
template<>
void bioseq_log<
    logging_level::debug,
    logging_target::file,
    std::string_view&,
    unsigned long long>(
    bioseq::common::logging_detail::format_with_location<
        std::string_view&,
        unsigned long long> format_location,
    std::string_view& route,
    unsigned long long&& total_rejections);

/**
 * @brief Emits a sequence-index payload summary through the specialized file-log path.
 *
 * @param[in] format_location Checked format string and caller coordinates.
 * @param[in] sequence_row_number Sequence row number.
 * @param[in] index_name Index identifier referenced for the duration of the call.
 * @param[in] payload_bytes Payload size in bytes.
 */
template<>
void bioseq_log<
    logging_level::debug,
    logging_target::file,
    unsigned long long,
    std::string_view&,
    unsigned long long>(
    bioseq::common::logging_detail::format_with_location<
        unsigned long long,
        std::string_view&,
        unsigned long long> format_location,
    unsigned long long&& sequence_row_number,
    std::string_view& index_name,
    unsigned long long&& payload_bytes);

}

/**
 * @brief Formats and submits a record using a raw PostgreSQL error level.
 *
 * @tparam p_argument_types Format argument types.
 * @param[in] postgres_error_level Raw PostgreSQL error level interpreted by the adapter.
 * @param[in] format_location Checked format string and caller coordinates.
 * @param[in] arguments Values forwarded to the selected sink.
 */
export template<typename... p_argument_types>
[[gnu::always_inline]] inline void bioseq_log_pg(
    int postgres_error_level,
    bioseq::common::logging_detail::format_with_location<
        std::type_identity_t<p_argument_types>...> format_location,
    p_argument_types&&... arguments)
{
    auto logging_sink = bioseq::common::logging_detail::get_postgres_logging_sink();
    if (logging_sink.write_postgres_message != nullptr) {
        if (logging_sink.is_postgres_error_level_enabled != nullptr &&
            !logging_sink.is_postgres_error_level_enabled(postgres_error_level)) {
            return;
        }

        fmt::memory_buffer message;
        bioseq::common::logging_detail::format_into(
            message,
            format_location.format_string,
            std::forward<p_argument_types>(arguments)...);
        logging_sink.write_postgres_message(
            postgres_error_level,
            make_bioseq_source_location(format_location.location),
            std::string_view(message.data(), message.size() - 1));
        return;
    }

    switch (bioseq::common::logging_detail::fallback_logging_level_for_postgres_error(
        postgres_error_level)) {
        case logging_level::error:
            bioseq::common::logging_detail::write_file_log<logging_level::error>(
                format_location,
                std::forward<p_argument_types>(arguments)...);
            break;
        case logging_level::warning:
            bioseq::common::logging_detail::write_file_log<logging_level::warning>(
                format_location,
                std::forward<p_argument_types>(arguments)...);
            break;
        case logging_level::info:
            bioseq::common::logging_detail::write_file_log<logging_level::info>(
                format_location,
                std::forward<p_argument_types>(arguments)...);
            break;
        case logging_level::debug:
        case logging_level::disabled:
            if (bioseq::common::logging_detail::is_debug_logging_enabled()) {
                bioseq::common::logging_detail::write_file_log<logging_level::debug>(
                    format_location,
                    std::forward<p_argument_types>(arguments)...);
            }
            break;
    }
}

/**
 * @brief Emits a record and immediately drains the file-backed queue.
 *
 * @tparam p_level Severity selected at compile time.
 * @tparam p_target Destination selected at compile time.
 * @tparam p_argument_types Format argument types.
 * @param[in] format_location Checked format string and caller coordinates.
 * @param[in] arguments Values forwarded to the selected sink.
 */
export template<
    logging_level p_level = logging_level::debug,
    logging_target p_target = logging_target::file,
    typename... p_argument_types>
[[gnu::always_inline]] inline void bioseq_log_progress(
    bioseq::common::logging_detail::format_with_location<
        std::type_identity_t<p_argument_types>...> format_location,
    p_argument_types&&... arguments)
{
    if (!is_bioseq_log_enabled<p_level, p_target>()) {
        return;
    }

    bioseq_log<p_level, p_target>(
        format_location,
        std::forward<p_argument_types>(arguments)...);

    if constexpr (p_target == logging_target::file) {
        fmtlog::poll(true);
    } else if (bioseq::common::logging_detail::get_postgres_logging_sink().write_message ==
               nullptr) {
        fmtlog::poll(true);
    }
}

/**
 * @brief Identifies a nullary callable that constructs and emits a lazy log record.
 *
 * @tparam p_producer Callable type.
 */
export template<class p_producer>
concept bioseq_log_lazy_producer =
    requires(p_producer&& producer) {
        { std::forward<p_producer>(producer)() } -> std::same_as<void>;
    };

/**
 * @brief Invokes a record producer only when its log target is enabled.
 *
 * @tparam p_level Severity selected at compile time.
 * @tparam p_target Destination selected at compile time.
 * @tparam p_producer Nullary callable satisfying bioseq_log_lazy_producer.
 * @param[in] producer Callable invoked at most once.
 */
export template<
    logging_level p_level = logging_level::debug,
    logging_target p_target = logging_target::file,
    bioseq_log_lazy_producer p_producer>
[[gnu::always_inline]] inline void bioseq_log_lazy(p_producer&& producer)
{
    if (is_bioseq_log_enabled<p_level, p_target>()) {
        std::forward<p_producer>(producer)();
    }
}

/**
 * @brief Tests runtime severity and destination gates for a structured report.
 *
 * @param[in] severity Runtime report severity.
 * @param[in] destination Runtime report destination.
 * @return True when the report should be constructed.
 */
inline bool is_bioseq_report_enabled_at_runtime(
    logging_level severity,
    logging_target destination) noexcept
{
    if (severity == logging_level::disabled) {
        return false;
    }
    if (severity == logging_level::debug &&
        !bioseq::common::logging_detail::is_debug_logging_enabled()) {
        return false;
    }
    if (destination == logging_target::postgres) {
        auto logging_sink = bioseq::common::logging_detail::get_postgres_logging_sink();
        if (logging_sink.is_logging_level_enabled != nullptr) {
            return logging_sink.is_logging_level_enabled(severity);
        }
    }
    return fmtlog::checkLogLevel(
        bioseq::common::logging_detail::to_fmtlog_level(severity));
}

/**
 * @brief Tests whether a structured report should be constructed.
 *
 * @tparam p_level Severity selected at compile time.
 * @tparam p_target Destination selected at compile time.
 * @return Current report gate state.
 */
export template<
    logging_level p_level = logging_level::error,
    logging_target p_target = logging_target::postgres>
[[nodiscard]] inline bool is_bioseq_report_enabled() noexcept
{
    return is_bioseq_report_enabled_at_runtime(p_level, p_target);
}

/**
 * @brief Tests whether a report at a raw PostgreSQL error level should be constructed.
 *
 * @param[in] postgres_error_level Raw PostgreSQL error level.
 * @return Current sink or file-fallback gate state.
 */
export [[nodiscard]] inline bool is_bioseq_report_pg_enabled(
    int postgres_error_level) noexcept
{
    auto logging_sink = bioseq::common::logging_detail::get_postgres_logging_sink();
    if (logging_sink.is_postgres_error_level_enabled != nullptr) {
        return logging_sink.is_postgres_error_level_enabled(postgres_error_level);
    }
    return is_bioseq_report_enabled_at_runtime(
        bioseq::common::logging_detail::fallback_logging_level_for_postgres_error(
            postgres_error_level),
        logging_target::file);
}

/**
 * @brief Invokes a structured-report producer only when its destination is enabled.
 *
 * @tparam p_level Severity selected at compile time.
 * @tparam p_target Destination selected at compile time.
 * @tparam p_producer Nullary report producer.
 * @param[in] producer Callable invoked at most once.
 */
export template<
    logging_level p_level = logging_level::error,
    logging_target p_target = logging_target::postgres,
    bioseq_log_lazy_producer p_producer>
[[gnu::always_inline]] inline void bioseq_report_lazy(p_producer&& producer)
{
    if (is_bioseq_report_enabled<p_level, p_target>()) {
        std::forward<p_producer>(producer)();
    }
}

/**
 * @brief Invokes a raw-PostgreSQL-level report producer only when enabled.
 *
 * @tparam p_producer Nullary report producer.
 * @param[in] postgres_error_level Raw PostgreSQL error level.
 * @param[in] producer Callable invoked at most once.
 */
export template<bioseq_log_lazy_producer p_producer>
[[gnu::always_inline]] inline void bioseq_report_pg_lazy(
    int postgres_error_level,
    p_producer&& producer)
{
    if (is_bioseq_report_pg_enabled(postgres_error_level)) {
        std::forward<p_producer>(producer)();
    }
}

/**
 * @brief Builds one structured diagnostic and emits it at scope exit.
 *
 * The builder owns its formatting buffers and is neither movable nor copyable. Instances
 * are intended for single-threaded, lexical use. The registered sink is snapshotted only
 * when gating or emitting. Disabled reports avoid formatting and allocation.
 *
 * @code{.cpp}
 * bioseq_report<logging_level::warning>()
 *     .with_sql_error_code(error_code)
 *     .with_message("unable to read {}", path)
 *     .with_hint("verify the input path");
 * @endcode
 */
export class bioseq_report_builder {
public:
    /**
     * @brief Initializes a gated report builder without formatting any fields.
     *
     * @param[in] destination Preferred report destination.
     * @param[in] fallback_severity Severity used by the file fallback.
     * @param[in] postgres_error_level Raw PostgreSQL level when supplied.
     * @param[in] has_postgres_error_level Whether the raw PostgreSQL level is meaningful.
     * @param[in] location Caller source coordinates.
     */
    bioseq_report_builder(
        logging_target destination,
        logging_level fallback_severity,
        int postgres_error_level,
        bool has_postgres_error_level,
        std::source_location location) noexcept
        : destination_(destination),
          fallback_severity_(fallback_severity),
          postgres_error_level_(postgres_error_level),
          has_postgres_error_level_(has_postgres_error_level),
          is_enabled_(has_postgres_error_level
                  ? is_bioseq_report_pg_enabled(postgres_error_level)
                  : is_bioseq_report_enabled_at_runtime(
                        fallback_severity,
                        destination)),
          source_location_(location)
    {}

    bioseq_report_builder(const bioseq_report_builder&) = delete;
    bioseq_report_builder& operator=(const bioseq_report_builder&) = delete;
    bioseq_report_builder(bioseq_report_builder&&) = delete;
    bioseq_report_builder& operator=(bioseq_report_builder&&) = delete;

    /**
     * @brief Sets the optional SQL error code.
     *
     * @param[in] sql_error_code PostgreSQL-compatible SQL error code.
     * @return This builder for fluent chaining.
     */
    bioseq_report_builder& with_sql_error_code(int sql_error_code) noexcept
    {
        if (!is_enabled_) {
            return *this;
        }
        sql_error_code_ = sql_error_code;
        has_sql_error_code_ = true;
        return *this;
    }

    /**
     * @brief Replaces the primary report message.
     *
     * @tparam p_argument_types Format argument types.
     * @param[in] format_string Compile-time checked format string.
     * @param[in] arguments Values interpolated into the message.
     * @return This builder for fluent chaining.
     */
    template<typename... p_argument_types>
    bioseq_report_builder& with_message(
        fmt::format_string<p_argument_types...> format_string,
        p_argument_types&&... arguments)
    {
        if (!is_enabled_) {
            return *this;
        }
        bioseq::common::logging_detail::format_into(
            message_buffer_,
            format_string,
            std::forward<p_argument_types>(arguments)...);
        has_message_ = true;
        return *this;
    }

    /**
     * @brief Replaces the optional report detail.
     *
     * @tparam p_argument_types Format argument types.
     * @param[in] format_string Compile-time checked format string.
     * @param[in] arguments Values interpolated into the detail.
     * @return This builder for fluent chaining.
     */
    template<typename... p_argument_types>
    bioseq_report_builder& with_detail(
        fmt::format_string<p_argument_types...> format_string,
        p_argument_types&&... arguments)
    {
        if (!is_enabled_) {
            return *this;
        }
        bioseq::common::logging_detail::format_into(
            detail_buffer_,
            format_string,
            std::forward<p_argument_types>(arguments)...);
        has_detail_ = true;
        return *this;
    }

    /**
     * @brief Replaces the optional report hint.
     *
     * @tparam p_argument_types Format argument types.
     * @param[in] format_string Compile-time checked format string.
     * @param[in] arguments Values interpolated into the hint.
     * @return This builder for fluent chaining.
     */
    template<typename... p_argument_types>
    bioseq_report_builder& with_hint(
        fmt::format_string<p_argument_types...> format_string,
        p_argument_types&&... arguments)
    {
        if (!is_enabled_) {
            return *this;
        }
        bioseq::common::logging_detail::format_into(
            hint_buffer_,
            format_string,
            std::forward<p_argument_types>(arguments)...);
        has_hint_ = true;
        return *this;
    }

    /** @brief Emits the completed report to PostgreSQL or the file fallback. */
    [[gnu::always_inline]] inline ~bioseq_report_builder()
    {
        if (!is_enabled_) {
            return;
        }

        bioseq_report_view view{
            .level = fallback_severity_,
            .postgres_error_level = postgres_error_level_,
            .has_postgres_error_level = has_postgres_error_level_,
            .sql_error_code = sql_error_code_,
            .has_sql_error_code = has_sql_error_code_,
            .message = has_message_
                ? std::string_view(message_buffer_.data(), message_buffer_.size() - 1)
                : std::string_view{},
            .detail = has_detail_
                ? std::string_view(detail_buffer_.data(), detail_buffer_.size() - 1)
                : std::string_view{},
            .hint = has_hint_
                ? std::string_view(hint_buffer_.data(), hint_buffer_.size() - 1)
                : std::string_view{},
            .location = make_bioseq_source_location(source_location_),
        };

        if (destination_ == logging_target::postgres) {
            auto logging_sink = bioseq::common::logging_detail::get_postgres_logging_sink();
            if (logging_sink.write_report != nullptr) {
                logging_sink.write_report(view);
                return;
            }
        }

        bioseq::common::logging_detail::write_report_to_file(view);
    }

private:
    logging_target destination_;
    logging_level fallback_severity_;
    int postgres_error_level_;
    bool has_postgres_error_level_;
    bool is_enabled_;
    int sql_error_code_ = 0;
    bool has_sql_error_code_ = false;
    bool has_message_ = false;
    bool has_detail_ = false;
    bool has_hint_ = false;
    std::source_location source_location_;
    fmt::memory_buffer message_buffer_;
    fmt::memory_buffer detail_buffer_;
    fmt::memory_buffer hint_buffer_;
};

/**
 * @brief Creates a structured report builder with compile-time severity and destination.
 *
 * @tparam p_level File-fallback severity.
 * @tparam p_target Preferred destination.
 * @param[in] location Caller coordinates. Defaults to the factory call site.
 * @return Non-copyable builder that emits at scope exit.
 */
export template<
    logging_level p_level = logging_level::error,
    logging_target p_target = logging_target::postgres>
[[nodiscard]] inline bioseq_report_builder bioseq_report(
    std::source_location location = std::source_location::current())
{
    return bioseq_report_builder{
        p_target,
        p_level,
        0,
        false,
        location,
    };
}

/**
 * @brief Creates a structured report builder for a raw PostgreSQL error level.
 *
 * @param[in] postgres_error_level Raw PostgreSQL error level.
 * @param[in] location Caller coordinates. Defaults to the factory call site.
 * @return Non-copyable builder that emits at scope exit.
 */
export [[nodiscard]] inline bioseq_report_builder bioseq_report_pg(
    int postgres_error_level,
    std::source_location location = std::source_location::current())
{
    return bioseq_report_builder{
        logging_target::postgres,
        bioseq::common::logging_detail::fallback_logging_level_for_postgres_error(
            postgres_error_level),
        postgres_error_level,
        true,
        location,
    };
}

/**
 * @brief Emits a message-only structured report with an SQL error code.
 *
 * @tparam p_level File-fallback severity.
 * @tparam p_target Preferred destination.
 * @tparam p_argument_types Format argument types.
 * @param[in] sql_error_code PostgreSQL-compatible SQL error code.
 * @param[in] format_location Checked format string and caller coordinates.
 * @param[in] arguments Values interpolated into the report message.
 */
export template<
    logging_level p_level = logging_level::error,
    logging_target p_target = logging_target::postgres,
    typename... p_argument_types>
[[gnu::always_inline]] inline void bioseq_report(
    int sql_error_code,
    bioseq::common::logging_detail::format_with_location<
        std::type_identity_t<p_argument_types>...> format_location,
    p_argument_types&&... arguments)
{
    auto report = bioseq_report<p_level, p_target>(format_location.location);
    report.with_sql_error_code(sql_error_code)
        .with_message(
            format_location.format_string,
            std::forward<p_argument_types>(arguments)...);
}

/**
 * @brief Configures fmtlog for tests or the extension's file fallback.
 *
 * @param[in] is_test_mode When true, writes synchronously to stderr with debug enabled.
 * Otherwise resolves BIOSEQ_LOG_FILE and falls back to /tmp/bioseq_loglog.log.
 */
export inline void configure_logging(const bool is_test_mode = false)
{
    if (is_test_mode) {
        bioseq_set_fallback_debug_logging_enabled(true);
        fmtlog::setLogLevel(fmtlog::DBG);
        fmtlog::setThreadName("test-main");
        fmtlog::setFlushDelay(0);
        fmtlog::setLogFile(stderr, false);
    } else {
        const char* log_file = std::getenv("BIOSEQ_LOG_FILE");
        if (log_file == nullptr || *log_file == '\0') {
            log_file = "/tmp/bioseq_loglog.log";
        }
        if (FILE* log_stream = std::fopen(log_file, "w"); log_stream != nullptr) {
            fmtlog::setLogFile(log_stream, true);
        } else {
            fmtlog::setLogFile(stderr, false);
        }
        fmtlog::setFlushDelay(0);
        fmtlog::setLogLevel(fmtlog::DBG);
        fmtlog::setThreadName("main");
    }
}
