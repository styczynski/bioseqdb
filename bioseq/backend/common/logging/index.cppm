/**
 * @file
 * @brief Sends Bioseq log messages and error reports to files or PostgreSQL.
 */
module;

#include <cstdio>

export module bioseq.common.logging;

export import :fmtlog_backend;
export import :formatters;

import bioseq.common.hashing;
import std;

/** @brief Severity used to filter a Bioseq log record. */
export enum class logging_level : std::uint8_t {
    debug = 0,
    info = 1,
    warning = 2,
    error = 3,
    disabled = 4,
};

/** @brief Where to send a log record. */
export enum class logging_target : std::uint8_t {
    file = 0,
    postgres = 1,
};

/** @brief Lowest severity compiled into the logging interface. */
inline constexpr logging_level k_minimum_compiled_logging_level = logging_level::debug;

/**
 * @brief Source file, function and position passed to logging callbacks.
 *
 * Pointers borrow compiler strings with static lifetime. Keep field order
 * unchanged because adapters use this layout across the module boundary.
 */
export struct bioseq_source_location {
    const char* file_name = "";
    const char* function_name = "";
    std::uint_least32_t line = 0;
    std::uint_least32_t column = 0;
};

/**
 * @brief Copies standard source coordinates into the Bioseq callback type.
 *
 * The file and function names borrow the compiler's static strings.
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
 * @brief Message, detail and hint borrowed for one logging callback.
 *
 * The callback must finish using the string views before returning. Keep
 * field order and representation compatible with the logging adapter.
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
 * @brief Callbacks that send common log messages to PostgreSQL.
 *
 * The adapter keeps its debug flag and callback code alive until all calls
 * using the sink have finished, including calls already running when the
 * sink is replaced or cleared. Callbacks must support concurrent calls.
 * Registration and clearing copy this table under a mutex. Keep its layout
 * compatible with the adapter.
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
 * @brief Keeps a checked format string together with its call site.
 * @tparam p_argument_types Types expected by the format string.
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
 * @brief Identifies a file log call by source position, format and severity.
 *
 * File and format strings must have static lifetime. Keep all fields in
 * equality and hashing so distinct calls receive separate fmtlog IDs.
 */
struct log_site_key {
    std::string_view file_name;
    std::string_view format_string;
    std::uint_least32_t line;
    std::uint_least32_t column;
    std::uint8_t severity;
};

/** @brief Hashes all fields that identify a logging call. */
struct log_site_key_hash {
    /**
     * @brief Combines the source position, format and severity into a table hash.
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

/** @brief Checks whether two keys identify the same logging call. */
struct log_site_key_equal {
    /**
     * @brief Compares every field used to identify a logging call.
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
 * @brief Stores the fmtlog ID and source text for one logging call.
 *
 * The table owns entries through unique_ptr, so their addresses survive
 * rehashing. A mutex protects first registration. Later calls read the
 * published ID from an atomic.
 */
struct log_site_entry {
    std::atomic<std::uint32_t> published_fmtlog_id{0};
    std::uint32_t registration_fmtlog_id = 0;
    std::mutex registration_mutex;
    std::string formatted_location;
};

/** Allows shared lookup and exclusive insertion in the log-site table. */
inline std::shared_mutex log_site_table_mutex;
/** Owns one registration entry per source position, format and severity. */
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
 * @brief Formats a source position as file:line:column for fmtlog.
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
 * @brief Finds or creates the fmtlog registration entry for a logging call.
 *
 * Lookups share a lock. A miss takes the write lock and checks again
 * before inserting, since another thread may have registered the call.
 *
 * @param[in] location Source coordinates with static-lifetime strings.
 * @param[in] severity Severity included in the lookup key.
 * @param[in] format_string Format text that must have static lifetime.
 * @return Entry owned by the process-wide table, with a stable address.
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
 * @brief Converts Bioseq severity to the matching fmtlog level.
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
 * @brief Checks the compiled severity threshold and excludes disabled records.
 */
constexpr bool is_compiled_level_enabled(logging_level severity) noexcept
{
    return severity != logging_level::disabled &&
           static_cast<std::uint8_t>(severity) >=
               static_cast<std::uint8_t>(k_minimum_compiled_logging_level);
}

/**
 * @brief Uses debug severity when a PostgreSQL level falls back to file logging.
 *
 * The supplied PostgreSQL level does not change this fallback.
 */
inline logging_level fallback_logging_level_for_postgres_error(
    [[maybe_unused]] int postgres_error_level) noexcept
{
    return logging_level::debug;
}

/**
 * @brief Copies the PostgreSQL callback table while holding its registration lock.
 */
inline postgres_logging_sink get_postgres_logging_sink() noexcept
{
    std::lock_guard sink_lock(postgres_sink_mutex);
    return postgres_sink;
}

/**
 * @brief Reads PostgreSQL's debug flag, or the fallback flag when none is installed.
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
 * @brief Checks the compiled severity threshold and runtime debug setting.
 *
 * Target-specific filters are checked separately.
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
 * @brief Logs with the cached fmtlog ID, registering the call once if needed.
 *
 * First registration holds the site mutex. Once published, the ID can
 * be read without taking that mutex.
 * @param[in,out] site Registration data for this call.
 * @param[in] emitter Logs using the supplied ID and assigns it on first use.
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
 * @brief Queues a file log after checking the fmtlog severity filter.
 *
 * Finds the call's registered ID and records its source position and timestamp.
 * @param[in] format_location Checked format string and call site.
 * @param[in] arguments Values copied or moved into fmtlog's queue.
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
 * @brief Queues an already formatted message at the selected severity.
 * @param[in] location Source coordinates.
 * @param[in] message Text to copy into the queue. No terminator is required.
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
 * @brief Queues formatted text using a severity selected at runtime.
 *
 * Debug messages also check the debug flag. Disabled messages are dropped.
 * @param[in] location Source coordinates.
 * @param[in] message Text to copy into the queue. No terminator is required.
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
 * @brief Joins a report's message, detail and hint for the file log.
 *
 * The queue copies the text before the temporary buffer is destroyed.
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
 * @param[in] logging_sink Callback table whose pointees remain alive through
 * all calls using them, including calls still running after replacement.
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
 * @brief Reads the installed PostgreSQL debug flag, or the fallback flag if absent.
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
 * @brief Checks whether the selected log level and destination are enabled.
 *
 * Use this before preparing expensive arguments. It checks the compiled
 * threshold, debug flag and the PostgreSQL or fmtlog level filter.
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
 * @brief Logs a formatted message with its source location.
 *
 * PostgreSQL receives enabled messages through its registered callback.
 * File logging queues the arguments for later formatting, and also handles
 * PostgreSQL messages when no matching callback is installed.
 *
 * @tparam p_level Message severity.
 * @tparam p_target Requested destination.
 * @param[in] format_location Checked format string and call site.
 * @param[in] arguments Values to format.
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
 * @brief File-log specialization used for route rejection counts.
 *
 * Shares the compiled implementation for a route string and count.
 * @param[in] format_location Checked format string and call site.
 * @param[in] route Route name.
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
 * @brief File-log specialization used for sequence-index payload sizes.
 *
 * Shares the compiled implementation for a row number, index name and size.
 * @param[in] format_location Checked format string and call site.
 * @param[in] sequence_row_number Sequence row number.
 * @param[in] index_name Index name.
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
 * @brief Logs using a PostgreSQL error level interpreted by the adapter.
 *
 * If no PostgreSQL callback is installed, the message uses the file log
 * as a debug record and follows the debug setting.
 * @param[in] postgres_error_level PostgreSQL error level.
 * @param[in] format_location Checked format string and call site.
 * @param[in] arguments Values to format.
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
 * @brief Logs a progress message and polls immediately when output goes to a file.
 *
 * Use where the caller needs progress output before the next regular poll.
 * @param[in] format_location Checked format string and call site.
 * @param[in] arguments Values to format.
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
 * @brief A callable that takes no arguments and returns void.
 */
export template<class p_producer>
concept bioseq_log_lazy_producer =
    requires(p_producer&& producer) {
        { std::forward<p_producer>(producer)() } -> std::same_as<void>;
    };

/**
 * @brief Calls a log producer once when the selected destination is enabled.
 *
 * Put expensive argument preparation inside the producer so disabled logs
 * skip that work.
 * @param[in] producer Callable taking no arguments and returning void.
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
 * @brief Checks the runtime level and destination before building a report.
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
 * @brief Checks whether a report at the selected level should be built.
 */
export template<
    logging_level p_level = logging_level::error,
    logging_target p_target = logging_target::postgres>
[[nodiscard]] inline bool is_bioseq_report_enabled() noexcept
{
    return is_bioseq_report_enabled_at_runtime(p_level, p_target);
}

/**
 * @brief Checks PostgreSQL's report filter, or the debug file fallback filter.
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
 * @brief Calls a report producer once when the selected destination is enabled.
 * @param[in] producer Callable taking no arguments and returning void.
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
 * @brief Calls a report producer once when its PostgreSQL level is enabled.
 *
 * Uses the file fallback filter if no PostgreSQL level callback exists.
 * @param[in] postgres_error_level PostgreSQL error level.
 * @param[in] producer Callable taking no arguments and returning void.
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
 * @brief Builds a report and sends it when the builder is destroyed.
 *
 * The builder owns the message buffers. Keep it on one thread within its
 * scope. It cannot be copied or moved. Disabled reports skip formatting,
 * and the current sink is read again when the completed report is sent.
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
     * @brief Chooses the report destination and checks whether it is enabled.
     *
     * No message fields are formatted yet.
     * @param[in] destination Requested output destination.
     * @param[in] fallback_severity Severity used for file output.
     * @param[in] postgres_error_level Optional PostgreSQL error level.
     * @param[in] has_postgres_error_level Whether that level was supplied.
     * @param[in] location Source position of the report.
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
     * @brief Sets the SQL error code if the report is enabled.
     * @return This builder, so more fields can be set in the same expression.
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
     * @brief Formats and replaces the report message if enabled.
     * @return This builder, so more fields can be set in the same expression.
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
     * @brief Formats and replaces the report detail if enabled.
     * @return This builder, so more fields can be set in the same expression.
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
     * @brief Formats and replaces the report hint if enabled.
     * @return This builder, so more fields can be set in the same expression.
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
 * @brief Starts a report that is sent when its builder is destroyed.
 * @tparam p_level Report severity, also used by the file fallback.
 * @tparam p_target Requested destination.
 * @param[in] location Source position, captured at this call by default.
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
 * @brief Starts a report with a PostgreSQL error level.
 *
 * The builder sends it on destruction. File fallback uses debug severity.
 * @param[in] postgres_error_level PostgreSQL error level.
 * @param[in] location Source position, captured at this call by default.
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
 * @brief Sends a report containing a message and SQL error code.
 * @param[in] sql_error_code PostgreSQL-compatible SQL error code.
 * @param[in] format_location Checked format string and call site.
 * @param[in] arguments Values to format into the message.
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
 * @brief Selects log output for tests or the extension.
 *
 * Test mode enables debug logging and sends polled records to stderr.
 * Otherwise opens BIOSEQ_LOG_FILE, using /tmp/bioseq_loglog.log when the
 * variable is empty, and falls back to stderr if opening fails. File
 * output is truncated when configuration succeeds.
 *
 * @param[in] is_test_mode Select test output and the test-main thread label.
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
