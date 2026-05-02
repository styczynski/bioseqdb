module;

#include "common.h"

#include <string>
#include <utility>
#include <concepts>

export module bioseq_io.errors;

import mlogging;

export namespace bioseq::io {

/// Concept: a callable that accepts a diagnostic string by rvalue.
/// Used for the `failure_fn` passed into `read_sequences_file`.
template <typename failure_fn_t>
concept failure_callback = std::invocable<failure_fn_t, std::string&&>;

/// Build a diagnostic via a compile-checked fmt format string and
/// dispatch to the user's failure callback. Wraps the boilerplate of
/// `failure_fn(fmt::format(...))` while keeping the format string
/// compile-time validated. Spec §3.5 / §15.1.
template <typename failure_fn_t, typename... Args>
inline void emit_failure(failure_fn_t& failure_fn,
                         fmt::format_string<Args...> fmt_str,
                         Args&&... args) {
    failure_fn(fmt::format(fmt_str, std::forward<Args>(args)...));
}

}  // namespace bioseq::io
