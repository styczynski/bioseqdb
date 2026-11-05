/**
 * @file errors.cppm
 * @brief Failure callback and formatted diagnostic helpers.
 */
module;

#include <concepts>
#include <cstdint>
#include <string>
#include <utility>

export module bioseq.io.errors;

import bioseq.common.logging;

export namespace bioseq::io {

/// Plain input classification; consumers decide how to report it at their boundary.
enum class input_failure_kind : std::uint8_t { other, invalid_quality_byte };

/// Can be called with a diagnostic string passed as an rvalue.
template <typename p_failure_function>
concept failure_callback = std::invocable<p_failure_function, std::string&&>;

/// Retain typed input failures when supported, preserving existing string-only sinks.
template <typename p_failure_function>
inline void report_failure(p_failure_function& failure_fn, std::string&& message,
                           input_failure_kind kind = input_failure_kind::other) {
    if constexpr (std::invocable<p_failure_function&, std::string&&, input_failure_kind>)
        failure_fn(std::move(message), kind);
    else
        failure_fn(std::move(message));
}

/// Format and dispatch one diagnostic through the supplied callback.
template <typename p_failure_function, typename... p_arguments>
inline void emit_failure(p_failure_function& failure_fn,
                         fmt::format_string<p_arguments...> fmt_str,
                         p_arguments&&... args) {
    report_failure(failure_fn, fmt::format(fmt_str, std::forward<p_arguments>(args)...));
}

template <typename p_failure_function, typename... p_arguments>
inline void emit_failure(p_failure_function& failure_fn, input_failure_kind kind,
                         fmt::format_string<p_arguments...> fmt_str,
                         p_arguments&&... args) {
    report_failure(failure_fn, fmt::format(fmt_str, std::forward<p_arguments>(args)...), kind);
}

} // namespace bioseq::io
