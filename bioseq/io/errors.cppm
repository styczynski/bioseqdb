/**
 * @file errors.cppm
 * @brief Failure callback and formatted diagnostic helpers.
 */
module;

#include <concepts>
#include <string>
#include <utility>

export module bioseq.io.errors;

import bioseq.common.logging;

export namespace bioseq::io {

/// Accepts ownership of one diagnostic string.
template <typename p_failure_function>
concept failure_callback = std::invocable<p_failure_function, std::string&&>;

/// Format and dispatch one diagnostic through the supplied callback.
template <typename p_failure_function, typename... p_arguments>
inline void emit_failure(p_failure_function& failure_fn,
                         fmt::format_string<p_arguments...> fmt_str,
                         p_arguments&&... args) {
    failure_fn(fmt::format(fmt_str, std::forward<p_arguments>(args)...));
}

} // namespace bioseq::io
