/*-------------------------------------------------------------------------
 *
 * pg_utils/logging.cppm
 *
 * Type-safe wrappers around PostgreSQL's elog/ereport machinery built on
 * top of {fmt}. Replaces the legacy printf-style format strings (%s, %d,
 * UINT64_FORMAT, ...) with C++26 fmt::format_string<...> arguments,
 * yielding compile-time format checking and rich {fmt} formatters for
 * project types.
 *
 * Public surface:
 *
 *   - pg_log(level, "fmt", args...)                  -- mirror of elog()
 *   - PgReport(level).code(c).msg(...).hint(...)     -- builder mirroring
 *                                                       multi-component
 *                                                       ereport(...).
 *   - JustLogDB("fmt", args...)                      -- DB-side counterpart
 *                                                       of JustLog(): goes
 *                                                       through pg_log when
 *                                                       building inside the
 *                                                       PostgreSQL extension,
 *                                                       falls back to JustLog
 *                                                       under -DNO_POSTGRES.
 *
 * Both pg_log and PgReport capture std::source_location at the call site
 * via a default argument, so PG's elog log lines reference the actual
 * caller (file/line/function) rather than the helper module. Formatted
 * payloads are routed through thread-local fmt::memory_buffer instances
 * so we do not heap-allocate per call and the strings remain alive across
 * the underlying errstart()/errfinish() pair (errmsg_internal copies into
 * ErrorContext, so the buffer can be reused on the next call).
 *
 *-------------------------------------------------------------------------
 */
module;

#include "common.h"

#include <source_location>
#include <type_traits>
#include <utility>

export module pg_utils.logging;

import mlogging;

/*
 * Internal helpers. Each call site owns its own fmt::memory_buffer so the
 * helpers are re-entrant: a formatter that itself triggers a pg_log/PgReport
 * cannot clobber a partially built outer message. fmt::memory_buffer has a
 * 500-byte small-buffer optimisation, so typical messages stay on the stack.
 *
 * PG's errmsg_internal() copies the formatted string into the current
 * ErrorContext, so the buffer can be safely destroyed once errfinish()
 * returns (and is never observed at all when errfinish() longjmps).
 */
namespace pg_logging_internal {

/*
 * Format the supplied {fmt}-style format/args into the given buffer and
 * append a terminating null byte so .data() can be passed directly to
 * the C printf-style PG entry points via the "%s" indirection.
 */
template<typename... Args>
[[gnu::always_inline]] inline void format_into(
    fmt::memory_buffer& target,
    fmt::format_string<Args...> fmt_string,
    Args&&... args)
{
    target.clear();
    fmt::format_to(std::back_inserter(target), fmt_string, std::forward<Args>(args)...);
    target.push_back('\0');
}

/*
 * Wrapper that pairs a fmt::format_string with the std::source_location of
 * the caller, captured through the consteval constructor's default
 * argument. Using std::type_identity_t<Args>... in the call sites prevents
 * the template parameter pack from interfering with the format string
 * deduction (the trailing parameter pack stays deducible while the format
 * string slot stays in a non-deduced context).
 */
template<typename... Args>
struct fmt_with_loc
{
    fmt::format_string<Args...> format_string;
    std::source_location location;

    template<typename SourceFmt>
        requires std::convertible_to<const SourceFmt&, fmt::format_string<Args...>>
    consteval fmt_with_loc(
        const SourceFmt& source_fmt,
        std::source_location captured = std::source_location::current()) noexcept
        : format_string(source_fmt), location(captured) {}
};

}  // namespace pg_logging_internal

#ifdef NO_POSTGRES

/*
 * NO_POSTGRES build path: the standalone aligner / test binary does not
 * have access to PG's elog machinery, so route every pg_log() call into
 * the existing JustLog() infrastructure. Level is accepted for API
 * symmetry but ignored: JustLog always logs at the debug level managed
 * by the fmtlog runtime.
 */
export template<typename... Args>
[[gnu::always_inline]] inline void pg_log(
    int /*elevel*/,
    pg_logging_internal::fmt_with_loc<std::type_identity_t<Args>...> fmt_loc,
    Args&&... args)
{
    fmt::memory_buffer message_buffer;
    pg_logging_internal::format_into(message_buffer,
                                     fmt_loc.format_string, std::forward<Args>(args)...);
    JustLog("{}", message_buffer.data());
}

#else

/*
 * PostgreSQL build path: emit a one-shot ereport equivalent to
 *   elog(elevel, "%s", formatted_message)
 * by manually invoking errstart()/errmsg_internal()/errfinish() so we
 * can supply the caller's std::source_location verbatim instead of the
 * helper module's. The "%s" indirection is required because PG's
 * errmsg_internal is itself a printf-style sink and our pre-formatted
 * payload may contain literal '%' characters.
 */
export template<typename... Args>
[[gnu::always_inline]] inline void pg_log(
    int elevel,
    pg_logging_internal::fmt_with_loc<std::type_identity_t<Args>...> fmt_loc,
    Args&&... args)
{
    if (!errstart(elevel, TEXTDOMAIN))
    {
        return;
    }
    fmt::memory_buffer message_buffer;
    pg_logging_internal::format_into(message_buffer,
                                     fmt_loc.format_string, std::forward<Args>(args)...);
    errmsg_internal("%s", message_buffer.data());
    errfinish(fmt_loc.location.file_name(),
              static_cast<int>(fmt_loc.location.line()),
              fmt_loc.location.function_name());
    if (elevel >= ERROR)
    {
        __builtin_unreachable();
    }
}

#endif

#ifndef NO_POSTGRES

/*
 * PgReport mirrors the multi-component form of ereport():
 *
 *   ereport(ERROR, (errcode(...), errmsg("..."), errhint("...")))
 *
 * becomes
 *
 *   PgReport(ERROR).code(...).msg("...").hint("...");
 *
 * The temporary's destructor invokes errstart()/errfinish() so callers
 * never have to remember a trailing .emit(): the report fires at the end
 * of the full expression. As with the original macro, levels >= ERROR
 * trigger a longjmp inside errfinish() and never return; the destructor
 * is annotated with __builtin_unreachable() to inform the compiler.
 *
 * Construction captures std::source_location via a default argument so
 * the recorded file/line/function point at the caller, not this header.
 */
export class PgReport
{
public:
    [[gnu::always_inline]] explicit PgReport(
        int elevel,
        std::source_location captured = std::source_location::current()) noexcept
        : elevel_(elevel), location_(captured) {}

    PgReport(const PgReport&) = delete;
    PgReport& operator=(const PgReport&) = delete;
    PgReport(PgReport&&) = delete;
    PgReport& operator=(PgReport&&) = delete;

    /* Attach a SQLSTATE error code (ERRCODE_*) to the report. */
    PgReport& code(int sqlerrcode) noexcept
    {
        sqlerrcode_ = sqlerrcode;
        has_sqlerrcode_ = true;
        return *this;
    }

    /* Primary message, equivalent to errmsg_internal(). */
    template<typename... Args>
    PgReport& msg(fmt::format_string<Args...> fmt_string, Args&&... args)
    {
        pg_logging_internal::format_into(message_buffer_, fmt_string, std::forward<Args>(args)...);
        has_message_ = true;
        return *this;
    }

    /* Optional supplementary detail line, equivalent to errdetail_internal(). */
    template<typename... Args>
    PgReport& detail(fmt::format_string<Args...> fmt_string, Args&&... args)
    {
        pg_logging_internal::format_into(detail_buffer_, fmt_string, std::forward<Args>(args)...);
        has_detail_ = true;
        return *this;
    }

    /* Optional remediation hint, equivalent to errhint(). */
    template<typename... Args>
    PgReport& hint(fmt::format_string<Args...> fmt_string, Args&&... args)
    {
        pg_logging_internal::format_into(hint_buffer_, fmt_string, std::forward<Args>(args)...);
        has_hint_ = true;
        return *this;
    }

    [[gnu::always_inline]] ~PgReport()
    {
        if (!errstart(elevel_, TEXTDOMAIN))
        {
            return;
        }
        if (has_sqlerrcode_)
        {
            ::errcode(sqlerrcode_);
        }
        if (has_message_)
        {
            ::errmsg_internal("%s", message_buffer_.data());
        }
        if (has_detail_)
        {
            ::errdetail_internal("%s", detail_buffer_.data());
        }
        if (has_hint_)
        {
            ::errhint("%s", hint_buffer_.data());
        }
        ::errfinish(location_.file_name(),
                    static_cast<int>(location_.line()),
                    location_.function_name());
        if (elevel_ >= ERROR)
        {
            __builtin_unreachable();
        }
    }

private:
    int elevel_;
    int sqlerrcode_ = 0;
    bool has_sqlerrcode_ = false;
    bool has_message_ = false;
    bool has_detail_ = false;
    bool has_hint_ = false;
    std::source_location location_;
    fmt::memory_buffer message_buffer_;
    fmt::memory_buffer detail_buffer_;
    fmt::memory_buffer hint_buffer_;
};

#else

/*
 * Stand-in PgReport for the NO_POSTGRES path so call sites compile in
 * both modes. All components funnel into JustLog at destruction time
 * (the message line is emitted, with detail/hint suffixed if supplied).
 */
export class PgReport
{
public:
    [[gnu::always_inline]] explicit PgReport(int /*elevel*/) noexcept {}
    PgReport(const PgReport&) = delete;
    PgReport& operator=(const PgReport&) = delete;
    PgReport(PgReport&&) = delete;
    PgReport& operator=(PgReport&&) = delete;

    PgReport& code(int /*sqlerrcode*/) noexcept { return *this; }

    template<typename... Args>
    PgReport& msg(fmt::format_string<Args...> fmt_string, Args&&... args)
    {
        pg_logging_internal::format_into(message_buffer_, fmt_string, std::forward<Args>(args)...);
        has_message_ = true;
        return *this;
    }

    template<typename... Args>
    PgReport& detail(fmt::format_string<Args...> fmt_string, Args&&... args)
    {
        pg_logging_internal::format_into(detail_buffer_, fmt_string, std::forward<Args>(args)...);
        has_detail_ = true;
        return *this;
    }

    template<typename... Args>
    PgReport& hint(fmt::format_string<Args...> fmt_string, Args&&... args)
    {
        pg_logging_internal::format_into(hint_buffer_, fmt_string, std::forward<Args>(args)...);
        has_hint_ = true;
        return *this;
    }

    ~PgReport()
    {
        if (!has_message_) {
            return;
        }
        if (has_detail_ && has_hint_) {
            JustLog("{} | DETAIL: {} | HINT: {}",
                    message_buffer_.data(), detail_buffer_.data(), hint_buffer_.data());
        } else if (has_detail_) {
            JustLog("{} | DETAIL: {}", message_buffer_.data(), detail_buffer_.data());
        } else if (has_hint_) {
            JustLog("{} | HINT: {}", message_buffer_.data(), hint_buffer_.data());
        } else {
            JustLog("{}", message_buffer_.data());
        }
    }

private:
    bool has_message_ = false;
    bool has_detail_ = false;
    bool has_hint_ = false;
    fmt::memory_buffer message_buffer_;
    fmt::memory_buffer detail_buffer_;
    fmt::memory_buffer hint_buffer_;
};

#endif

/*
 * JustLogDB is the Postgres counterpart of JustLog: it routes through
 * pg_log when the extension is built against PG (so messages land in the
 * server log) and falls back to JustLog when -DNO_POSTGRES is in effect.
 *
 * The PG variant logs at LOG level so the line is recorded server-side
 * but never propagated to the client connection.
 */
#ifdef NO_POSTGRES
#define JustLogDB(...) JustLog(__VA_ARGS__)
#else
#define JustLogDB(...) pg_log(LOG, __VA_ARGS__)
#endif
